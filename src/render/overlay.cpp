#include "render/overlay.h"

#include "common/log.h"
#include "hooks/d3d_hooks.h"
#include "common/config.h"

#include <cstdio>

namespace pvr::overlay {

// Two transparent layers per frame:
//  hud  - the status bar etc., drawn inside the world pass (redirected by stereo's HudDraw hook)
//  menu - everything the engine draws into the scene target after the world pass (menus, messages)
struct Layer {
    const char *name;
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    UINT w = 0, h = 0;
    bool active = false;   // drawn into this frame
    bool cleared = false;  // cleared this frame
};
static Layer g_hud{"hud"}, g_menu{"menu"};
static ID3D11RenderTargetView *g_sceneRtv = nullptr;  // our view of the scene target, restored at End
static ID3D11Texture2D *g_sceneTex = nullptr;

static bool Ensure(Layer &L, ID3D11Texture2D *like) {
    D3D11_TEXTURE2D_DESC d;
    like->GetDesc(&d);
    if (L.tex && L.w == d.Width && L.h == d.Height) return true;
    if (L.srv) L.srv->Release(), L.srv = nullptr;
    if (L.rtv) L.rtv->Release(), L.rtv = nullptr;
    if (L.tex) L.tex->Release(), L.tex = nullptr;
    D3D11_TEXTURE2D_DESC c{};
    c.Width = d.Width;
    c.Height = d.Height;
    c.MipLevels = 1;
    c.ArraySize = 1;
    c.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    c.SampleDesc = {1, 0};
    c.Usage = D3D11_USAGE_DEFAULT;
    c.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device *dev = GameDevice();
    if (FAILED(dev->CreateTexture2D(&c, nullptr, &L.tex))) return false;
    dev->CreateRenderTargetView(L.tex, nullptr, &L.rtv);
    dev->CreateShaderResourceView(L.tex, nullptr, &L.srv);
    L.w = d.Width;
    L.h = d.Height;
    PVR_LOG("overlay: %s target %ux%u", L.name, L.w, L.h);
    return L.rtv && L.srv;
}

static void ClearOnce(ID3D11DeviceContext *ctx, Layer &L) {
    if (L.cleared) return;
    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(L.rtv, clear);
    L.cleared = true;
}

ID3D11Texture2D *Target() { return g_menu.tex; }
ID3D11Texture2D *SceneTarget() { return g_sceneTex; }

ID3D11RenderTargetView *Prepare(ID3D11DeviceContext *ctx, ID3D11Texture2D *like) {
    if (!like || !Ensure(g_hud, like)) return nullptr;
    ClearOnce(ctx, g_hud);
    g_hud.active = true;
    return g_hud.rtv;
}

void Begin(ID3D11DeviceContext *ctx, ID3D11Texture2D *sceneRT) {
    if (!sceneRT || !Ensure(g_menu, sceneRT)) return;
    ClearOnce(ctx, g_menu);
    if (g_sceneTex != sceneRT) {
        if (g_sceneRtv) g_sceneRtv->Release(), g_sceneRtv = nullptr;
        GameDevice()->CreateRenderTargetView(sceneRT, nullptr, &g_sceneRtv);
        g_sceneTex = sceneRT;
    }
    SetRenderTargetRemap(sceneRT, g_menu.rtv);
    {  // diagnostics: what is drawn into the overlay after the world pass
        static ULONGLONG s_next = 0;
        static int s_logs = 0;
        if (s_logs < 3 && GetTickCount64() >= s_next) {
            ++s_logs, s_next = GetTickCount64() + 20000;
            LogDraws(40, "post");
        }
    }
    // the engine may assume the scene target is still bound and skip rebinding it
    ID3D11RenderTargetView *cur = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    ctx->OMGetRenderTargets(1, &cur, &dsv);
    if (cur) {
        ID3D11Resource *r = nullptr;
        cur->GetResource(&r);
        if (r == sceneRT) ctx->OMSetRenderTargets(1, &cur, dsv);  // goes through the remap
        if (r) r->Release();
        cur->Release();
    }
    if (dsv) dsv->Release();
    g_menu.active = true;
}

// diagnostics: every 20 s (max 4) write the layers as raw RGBA8 to <game>\pvr_<layer>_N.raw
static void Dump(ID3D11DeviceContext *ctx, Layer &L, int n) {
    D3D11_TEXTURE2D_DESC d;
    L.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    if (FAILED(GameDevice()->CreateTexture2D(&d, nullptr, &st))) return;
    ctx->CopyResource(st, L.tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%spvr_%S_%d.raw", GameDir().c_str(), L.name, n);
        FILE *f = _wfopen(path, L"wb");
        if (f) {
            for (UINT y = 0; y < d.Height; ++y) fwrite((uint8_t *)m.pData + y * m.RowPitch, 4, d.Width, f);
            fclose(f);
        }
        ctx->Unmap(st, 0);
        PVR_LOG("overlay: dumped %s %ux%u #%d", L.name, d.Width, d.Height, n);
    }
    st->Release();
}
static void MaybeDump(ID3D11DeviceContext *ctx) {
    static ULONGLONG s_next = GetTickCount64() + 30000;
    static int s_n = 0;
    if (!Cfg().dumpLayers || s_n >= 4 || GetTickCount64() < s_next || !g_hud.active) return;
    s_next = GetTickCount64() + 20000;
    Dump(ctx, g_hud, s_n);
    if (g_menu.active) Dump(ctx, g_menu, s_n);
    ++s_n;
}

static void Finish(Layer &L, Out &o) {
    o = {};
    if (L.active) o.srv = L.srv, o.w = L.w, o.h = L.h;
    L.active = false;
    L.cleared = false;
}

void End(Out &hud, Out &menu) {
    SetRenderTargetRemap(nullptr, nullptr);
    LogDraws(0, "");
    ID3D11DeviceContext *ctx = nullptr;
    GameDevice()->GetImmediateContext(&ctx);
    if (g_menu.active && g_sceneRtv) {
        // if our overlay is still bound, give the engine back the target it thinks is bound
        ID3D11RenderTargetView *cur = nullptr;
        ID3D11DepthStencilView *dsv = nullptr;
        ctx->OMGetRenderTargets(1, &cur, &dsv);
        if (cur == g_menu.rtv) ctx->OMSetRenderTargets(1, &g_sceneRtv, nullptr);
        if (cur) cur->Release();
        if (dsv) dsv->Release();
    }
    MaybeDump(ctx);
    ctx->Release();
    Finish(g_hud, hud);
    Finish(g_menu, menu);
}

}  // namespace pvr::overlay
