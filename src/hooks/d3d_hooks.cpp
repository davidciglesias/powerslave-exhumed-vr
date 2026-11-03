#include "hooks/d3d_hooks.h"

#include <vector>
#include <algorithm>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <MinHook.h>

#include <atomic>

#include "common/log.h"
#include "xr/xr_session.h"

namespace pvr {

static ID3D11Device *g_device = nullptr;
ID3D11Device *GameDevice() { return g_device; }

using PFN_CreateDevice = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                          UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
using PFN_CreateDeviceAndSwapChain = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                      const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                      const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
                                                      ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
using PFN_Present = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

static PFN_CreateDevice o_CreateDevice = nullptr;
static PFN_CreateDeviceAndSwapChain o_CreateDeviceAndSwapChain = nullptr;
static PFN_Present o_Present = nullptr;
static PFN_Present1 o_Present1 = nullptr;
static bool g_presentHooked = false;

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) xr::OnPresent(sc);
    return o_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS *p) {
    if (!(flags & DXGI_PRESENT_TEST)) xr::OnPresent(sc);
    return o_Present1(sc, sync, flags, p);
}

// Create a throwaway swapchain on the game's device to read DXGI's Present vtable slots.
static void HookPresent(ID3D11Device *dev) {
    if (g_presentHooked || !dev) return;
    IDXGIDevice *dxgiDev = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return;
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PVRDummy";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"PVRDummy", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = 64;
    d.Height = 64;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc = nullptr;
    HRESULT hr = factory ? factory->CreateSwapChainForHwnd(dev, hwnd, &d, nullptr, nullptr, &sc) : E_FAIL;
    if (SUCCEEDED(hr) && sc) {
        void **vt = *reinterpret_cast<void ***>(sc);
        MH_CreateHook(vt[8], (void *)hk_Present, (void **)&o_Present);     // IDXGISwapChain::Present
        MH_CreateHook(vt[22], (void *)hk_Present1, (void **)&o_Present1);  // IDXGISwapChain1::Present1
        MH_EnableHook(MH_ALL_HOOKS);
        g_presentHooked = true;
        PVR_LOG("Present hooked (%p / %p)", vt[8], vt[22]);
        sc->Release();
    } else {
        PVR_LOG("dummy swapchain failed hr=0x%08X", (unsigned)hr);
    }
    DestroyWindow(hwnd);
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    dxgiDev->Release();
}

// ---- diagnostics: count immediate-context draws, detect deferred contexts ----
std::atomic<int> g_drawCount{0};
// render-target tracing: while active, every draw is attributed to the currently bound RT 0
struct RTCount { ID3D11Resource *rt; int draws; };
struct Tracer { bool on = false; RTCount counts[24]; int n = 0; ID3D11Resource *last = nullptr; };
static Tracer g_tr[2];  // 0 = per-eye scene capture, 1 = whole-frame diagnostics
static ID3D11Resource *g_curRT = nullptr;  // not ref-counted; identity only
// optional pixel-shader override applied to every draw while set (used for the VR sky)
static ID3D11PixelShader *g_psOverride = nullptr;
static ID3D11Buffer *g_cbOverride = nullptr;
static UINT g_cbSlot = 0;
void SetPixelShaderOverride(ID3D11PixelShader *ps, ID3D11Buffer *cb, UINT slot) {
    g_psOverride = ps;
    g_cbOverride = cb;
    g_cbSlot = slot;
}
struct PsSwap {
    ID3D11DeviceContext *c;
    ID3D11PixelShader *ps = nullptr;
    ID3D11ClassInstance *ci[16];
    UINT nci = 16;
    ID3D11Buffer *cb = nullptr;
    bool on;
    explicit PsSwap(ID3D11DeviceContext *ctx) : c(ctx), on(g_psOverride != nullptr) {
        if (!on) return;
        c->PSGetShader(&ps, ci, &nci);
        c->PSGetConstantBuffers(g_cbSlot, 1, &cb);
        c->PSSetShader(g_psOverride, nullptr, 0);
        c->PSSetConstantBuffers(g_cbSlot, 1, &g_cbOverride);
    }
    ~PsSwap() {
        if (!on) return;
        c->PSSetShader(ps, nci ? ci : nullptr, nci);
        c->PSSetConstantBuffers(g_cbSlot, 1, &cb);
        if (ps) ps->Release();
        for (UINT i = 0; i < nci; ++i)
            if (ci[i]) ci[i]->Release();
        if (cb) cb->Release();
    }
};
static void Attribute() {
    for (auto &t : g_tr) {
        if (!t.on) continue;
        t.last = g_curRT;
        int k = 0;
        for (; k < t.n; ++k)
            if (t.counts[k].rt == g_curRT) { ++t.counts[k].draws; break; }
        if (k == t.n && t.n < 24) t.counts[t.n++] = {g_curRT, 1};
    }
}
void TraceBegin(int slot, ID3D11Resource *initial) {
    g_tr[slot].on = true;
    g_tr[slot].n = 0;
    g_tr[slot].last = nullptr;
    if (initial) g_curRT = initial;
}
ID3D11Resource *TraceLast(int slot) { return g_tr[slot].last; }
ID3D11Resource *TraceEnd(int slot, bool log) {
    Tracer &tr = g_tr[slot];
    tr.on = false;
    RTCount *g_rtCounts = tr.counts;
    int g_nRt = tr.n;
    ID3D11Resource *best = nullptr;
    int bestN = 0;
    for (int i = 0; i < g_nRt; ++i) {
        if (log) {
            D3D11_TEXTURE2D_DESC d{};
            ID3D11Texture2D *t = nullptr;
            if (g_rtCounts[i].rt && SUCCEEDED(g_rtCounts[i].rt->QueryInterface(IID_PPV_ARGS(&t)))) { t->GetDesc(&d); t->Release(); }
            PVR_LOG("trace%d: rt %p %ux%u fmt %d samples %u: %d draws", slot, (void *)g_rtCounts[i].rt, d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, g_rtCounts[i].draws);
        }
        if (g_rtCounts[i].rt && g_rtCounts[i].draws > bestN) { best = g_rtCounts[i].rt; bestN = g_rtCounts[i].draws; }
    }
    return best;
}
using PFN_OMSetRT = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
using PFN_DrawIdxInst = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInst = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
static PFN_OMSetRT o_OMSetRT;
static PFN_DrawIdxInst o_DrawIdxInst;
static PFN_DrawInst o_DrawInst;
static ID3D11RenderTargetView *g_rtRedirect = nullptr;
static ID3D11Resource *g_remapFrom = nullptr;
static ID3D11RenderTargetView *g_remapTo = nullptr;
void SetRenderTargetRemap(ID3D11Resource *from, ID3D11RenderTargetView *to) {
    g_remapFrom = to ? from : nullptr;
    g_remapTo = to;
}
void SetRenderTargetRedirect(ID3D11RenderTargetView *rtv) { g_rtRedirect = rtv; }
static void STDMETHODCALLTYPE hk_OMSetRT(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) {
    if (g_rtRedirect) {  // everything goes to the redirect target, no depth
        g_curRT = nullptr;
        o_OMSetRT(c, 1, &g_rtRedirect, nullptr);
        return;
    }
    if (g_remapFrom && n && rtv && rtv[0]) {  // one specific target -> another
        ID3D11Resource *r = nullptr;
        rtv[0]->GetResource(&r);
        if (r) r->Release();
        if (r == g_remapFrom) {
            g_curRT = nullptr;
            o_OMSetRT(c, 1, &g_remapTo, nullptr);
            return;
        }
    }
    g_curRT = nullptr;  // always tracked so tracers can start mid-frame
    if (n && rtv && rtv[0]) { rtv[0]->GetResource(&g_curRT); if (g_curRT) g_curRT->Release(); }
    o_OMSetRT(c, n, rtv, dsv);
}
// diagnostics: log the next N draws (viewport, scissor, counts, shaders, blend)
static int g_drawLog = 0;
static const char *g_drawTag = "";
void LogDraws(int n, const char *tag) { g_drawLog = n; g_drawTag = tag; }
// copy `bytes` of a GPU buffer starting at `off` to the CPU (diagnostics only)
static bool ReadBuf(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off, UINT bytes, std::vector<uint8_t> &out) {
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    if (off >= d.ByteWidth) return false;
    bytes = (std::min)(bytes, d.ByteWidth - off);
    D3D11_BUFFER_DESC s{};
    s.ByteWidth = bytes;
    s.Usage = D3D11_USAGE_STAGING;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *st = nullptr;
    ID3D11Device *dev = nullptr;
    c->GetDevice(&dev);
    HRESULT hr = dev->CreateBuffer(&s, nullptr, &st);
    dev->Release();
    if (FAILED(hr)) return false;
    D3D11_BOX box{off, 0, 0, off + bytes, 1, 1};
    c->CopySubresourceRegion(st, 0, 0, 0, 0, b, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) out.assign((uint8_t *)m.pData, (uint8_t *)m.pData + bytes), c->Unmap(st, 0);
    st->Release();
    return ok;
}
// log vertex positions / first dwords of an indexed draw (diagnostics)
static void LogGeom(ID3D11DeviceContext *c, UINT count, UINT start, INT base) {
    ID3D11Buffer *ib = nullptr, *vb = nullptr;
    DXGI_FORMAT fmt;
    UINT ioff = 0, stride = 0, voff = 0;
    c->IAGetIndexBuffer(&ib, &fmt, &ioff);
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    std::vector<uint8_t> ix, vx;
    UINT isz = fmt == DXGI_FORMAT_R16_UINT ? 2 : 4;
    if (ib && vb && stride && ReadBuf(c, ib, ioff + start * isz, count * isz, ix)) {
        UINT lo = ~0u, hi = 0;
        for (UINT i = 0; i * isz < ix.size(); ++i) {
            UINT v = isz == 2 ? ((uint16_t *)ix.data())[i] : ((uint32_t *)ix.data())[i];
            lo = (std::min)(lo, v), hi = (std::max)(hi, v);
        }
        if (lo <= hi && ReadBuf(c, vb, voff + (UINT)((INT)lo + base) * stride, (hi - lo + 1) * stride, vx)) {
            char line[512];
            int n = 0;
            for (UINT v = 0; v <= hi - lo && v < 12 && (v + 1) * stride <= vx.size(); ++v) {
                const float *f = (const float *)(vx.data() + v * stride);
                const uint32_t *u = (const uint32_t *)(vx.data() + v * stride);
                n += snprintf(line + n, sizeof(line) - n, " (%.0f,%.0f %08x)", f[0], f[1], stride >= 24 ? u[stride / 4 - 1] : 0);
                if (n > 440) break;
            }
            PVR_LOG("  geom stride %u verts %u..%u:%s", stride, lo, hi, line);
        }
    }
    D3D11_RECT sc{};
    UINT ns = 1;
    c->RSGetScissorRects(&ns, &sc);
    ID3D11RasterizerState *rs = nullptr;
    c->RSGetState(&rs);
    D3D11_RASTERIZER_DESC rd{};
    if (rs) rs->GetDesc(&rd), rs->Release();
    PVR_LOG("  scissor %d (%ld,%ld)-(%ld,%ld) cull %d", rd.ScissorEnable, sc.left, sc.top, sc.right, sc.bottom, rd.CullMode);
    if (ib) ib->Release();
    if (vb) vb->Release();
}
static UINT g_geomStart = 0;
static INT g_geomBase = 0;
static void LogDraw(ID3D11DeviceContext *c, const char *kind, UINT cnt) {
    if (g_drawLog <= 0) return;
    --g_drawLog;
    D3D11_VIEWPORT vp{};
    UINT nv = 1;
    c->RSGetViewports(&nv, &vp);
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11BlendState *bs = nullptr;
    float bf[4];
    UINT sm = 0;
    c->OMGetBlendState(&bs, bf, &sm);
    D3D11_BLEND_DESC bd{};
    if (bs) bs->GetDesc(&bd);
    ID3D11PixelShader *ps = nullptr;
    c->PSGetShader(&ps, nullptr, nullptr);
    ID3D11DepthStencilState *ds = nullptr;
    UINT sref = 0;
    c->OMGetDepthStencilState(&ds, &sref);
    D3D11_DEPTH_STENCIL_DESC dd{};
    if (ds) ds->GetDesc(&dd);
    const auto &rt0 = bd.RenderTarget[0];
    PVR_LOG("draw[%s] %s n=%u vp(%.0f,%.0f %.0fx%.0f) rtv=%p dsv=%p ps=%p blend=%d src=%d dst=%d srcA=%d dstA=%d wm=%x depth=%d stencil=%d",
            g_drawTag, kind, cnt, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, (void *)rtv, (void *)dsv, (void *)ps,
            rt0.BlendEnable, rt0.SrcBlend, rt0.DestBlend, rt0.SrcBlendAlpha, rt0.DestBlendAlpha, rt0.RenderTargetWriteMask,
            dd.DepthEnable, dd.StencilEnable);
    if (kind[0] == 'D' && kind[1] == 'I' && kind[2] == 'd') LogGeom(c, cnt, g_geomStart, g_geomBase);
    IUnknown *rel[] = {rtv, dsv, bs, ps, ds};
    for (auto *x : rel)
        if (x) x->Release();
}
static void STDMETHODCALLTYPE hk_DrawIdxInst(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e, UINT f) { ++g_drawCount; Attribute(); LogDraw(c, "DII", a * b); PsSwap sw(c); o_DrawIdxInst(c, a, b, d, e, f); }
static void STDMETHODCALLTYPE hk_DrawInst(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e) { ++g_drawCount; Attribute(); LogDraw(c, "DI", a * b); PsSwap sw(c); o_DrawInst(c, a, b, d, e); }
using PFN_DrawIndexed = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using PFN_ExecCL = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
using PFN_CreateDeferred = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, UINT, ID3D11DeviceContext **);
static PFN_DrawIndexed o_DrawIndexed;
static PFN_Draw o_Draw;
static PFN_ExecCL o_ExecCL;
static PFN_CreateDeferred o_CreateDeferred;
static void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext *c, UINT a, UINT b, INT d) {
    ++g_drawCount;
    Attribute();
    g_geomStart = b, g_geomBase = d;
    LogDraw(c, "DIdx", a);
    PsSwap sw(c);
    o_DrawIndexed(c, a, b, d);
}
static void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext *c, UINT a, UINT b) {
    ++g_drawCount;
    Attribute();
    LogDraw(c, "D", a);
    PsSwap sw(c);
    o_Draw(c, a, b);
}
static void STDMETHODCALLTYPE hk_ExecCL(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL r) {
    static int n = 0;
    if (n++ < 5) PVR_LOG("diag: ExecuteCommandList called");
    o_ExecCL(c, l, r);
}
static HRESULT STDMETHODCALLTYPE hk_CreateDeferred(ID3D11Device *d, UINT f, ID3D11DeviceContext **c) {
    PVR_LOG("diag: CreateDeferredContext called");
    return o_CreateDeferred(d, f, c);
}

static void OnDevice(ID3D11Device *dev, const char *via) {
    if (!dev) return;
    if (!g_device) {
        g_device = dev;
        g_device->AddRef();
        PVR_LOG("captured D3D11 device %p via %s, FL=0x%X flags=0x%X", dev, via, dev->GetFeatureLevel(),
                dev->GetCreationFlags());
        ID3D11DeviceContext *ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        void **cvt = *reinterpret_cast<void ***>(ctx);
        void **dvt = *reinterpret_cast<void ***>(dev);
        MH_CreateHook(cvt[12], (void *)hk_DrawIndexed, (void **)&o_DrawIndexed);
        MH_CreateHook(cvt[13], (void *)hk_Draw, (void **)&o_Draw);
        MH_CreateHook(cvt[58], (void *)hk_ExecCL, (void **)&o_ExecCL);
        MH_CreateHook(cvt[20], (void *)hk_DrawIdxInst, (void **)&o_DrawIdxInst);
        MH_CreateHook(cvt[21], (void *)hk_DrawInst, (void **)&o_DrawInst);
        MH_CreateHook(cvt[33], (void *)hk_OMSetRT, (void **)&o_OMSetRT);
        MH_CreateHook(dvt[27], (void *)hk_CreateDeferred, (void **)&o_CreateDeferred);
        MH_EnableHook(MH_ALL_HOOKS);
        ctx->Release();
    }
    HookPresent(dev);
}

static HRESULT WINAPI hk_CreateDevice(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE s, UINT f,
                                      const D3D_FEATURE_LEVEL *fl, UINT n, UINT v, ID3D11Device **d,
                                      D3D_FEATURE_LEVEL *ofl, ID3D11DeviceContext **c) {
    HRESULT hr = o_CreateDevice(a, t, s, f, fl, n, v, d, ofl, c);
    if (SUCCEEDED(hr) && d && *d) OnDevice(*d, "D3D11CreateDevice");
    return hr;
}

static HRESULT WINAPI hk_CreateDeviceAndSwapChain(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE s, UINT f,
                                                  const D3D_FEATURE_LEVEL *fl, UINT n, UINT v,
                                                  const DXGI_SWAP_CHAIN_DESC *sd, IDXGISwapChain **sc,
                                                  ID3D11Device **d, D3D_FEATURE_LEVEL *ofl,
                                                  ID3D11DeviceContext **c) {
    HRESULT hr = o_CreateDeviceAndSwapChain(a, t, s, f, fl, n, v, sd, sc, d, ofl, c);
    if (SUCCEEDED(hr) && d && *d) OnDevice(*d, "D3D11CreateDeviceAndSwapChain");
    return hr;
}

void InstallD3DHooks() {
    MH_Initialize();
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    if (!d3d) {
        PVR_LOG("d3d11.dll not loadable");
        return;
    }
    MH_CreateHook((void *)GetProcAddress(d3d, "D3D11CreateDevice"), (void *)hk_CreateDevice, (void **)&o_CreateDevice);
    MH_CreateHook((void *)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain"), (void *)hk_CreateDeviceAndSwapChain,
                  (void **)&o_CreateDeviceAndSwapChain);
    MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    PVR_LOG("d3d11 create hooks: %d", (int)st);
}

}  // namespace pvr
