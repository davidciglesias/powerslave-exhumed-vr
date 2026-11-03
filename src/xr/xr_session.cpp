// OpenXR session on the game's D3D11 device.
// Frame pipelining: at each game Present we end the XR frame the game just rendered, then
// wait/begin the next one and locate its views, which the stereo world-draw hook uses.
// Gameplay frames are submitted as a stereo projection layer; anything else (menus, movies)
// falls back to the flat backbuffer on a world-locked quad.
#include "xr/xr_session.h"

#include <algorithm>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <mutex>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "hooks/d3d_hooks.h"
#include "render/blit.h"
#include "render/gauges.h"
#include "game/hud_state.h"
#include "render/overlay.h"
#include "input/pad_state.h"
#include "common/vrmath.h"
#include "xr/fake_xr.h"
#include "xr/xr_input.h"

namespace pvr::xr {

static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = XR_NULL_SYSTEM_ID;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_local = XR_NULL_HANDLE;
static XrSpace g_viewSpace = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;
static bool g_failed = false;

struct Chain {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;  // source format key
    DXGI_FORMAT scFmt = DXGI_FORMAT_UNKNOWN;
};
static Chain g_quad, g_eyeChain[2];
static ID3D11Texture2D *g_resolve = nullptr;  // for MSAA backbuffers

// eye images captured during the frame (owned copies with SRVs for the blit)
struct EyeImg {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    uint64_t frame = 0;
};
static EyeImg g_eyeImg[2];
static std::vector<ID3D11RenderTargetView *> g_eyeRtv[2];

// frame state shared with the render hook
static std::mutex g_frameMtx;
static bool g_frameBegun = false;
static XrFrameState g_fs{XR_TYPE_FRAME_STATE};
static XrView g_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
static bool g_viewsValid = false;
static uint64_t g_frameIndex = 1;
static bool g_forceFlat = false;

bool Running() { return g_running; }

#define XR_CHECK(x)                                                     \
    do {                                                                \
        XrResult _r = (x);                                              \
        if (XR_FAILED(_r)) {                                            \
            PVR_LOG("%s failed: %d (%s:%d)", #x, (int)_r, __FILE__, __LINE__); \
            return false;                                               \
        }                                                               \
    } while (0)

static bool InitInstance(ID3D11Device *dev) {
    uint32_t nExt = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &nExt, nullptr);
    std::vector<XrExtensionProperties> avail(nExt, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, nExt, &nExt, avail.data());
    auto has = [&](const char *n) {
        for (auto &e : avail)
            if (!strcmp(e.extensionName, n)) return true;
        return false;
    };
    std::vector<const char *> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (has("XR_VALVE_frame_controller_interaction")) exts.push_back("XR_VALVE_frame_controller_interaction");
    PVR_LOG("frame controller extension: %d", has("XR_VALVE_frame_controller_interaction"));

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "PowerslaveVR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "KEX4");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    XR_CHECK(xrCreateInstance(&ci, &g_inst));

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    PVR_LOG("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
            XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(g_inst, &sg, &g_sys));

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(getReq(g_inst, g_sys, &req));

    IDXGIDevice *dxgiDev = nullptr;
    IDXGIAdapter *ad = nullptr;
    DXGI_ADAPTER_DESC ad_desc{};
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
        dxgiDev->GetAdapter(&ad);
        ad->GetDesc(&ad_desc);
        ad->Release();
        dxgiDev->Release();
    }
    bool same = memcmp(&ad_desc.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0;
    PVR_LOG("game adapter %ls, matches XR adapter: %d, minFL=0x%X", ad_desc.Description, same,
            req.minFeatureLevel);

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_sys;
    XR_CHECK(xrCreateSession(g_inst, &sci, &g_sess));

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(g_sess, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrResult vr = xrCreateReferenceSpace(g_sess, &rs, &g_viewSpace);
    PVR_LOG("OpenXR session created (VIEW space result %d, handle %p)", (int)vr, (void *)g_viewSpace);
    if (Cfg().virtualPad) InputInit(g_inst, g_sess);
    return true;
}

static DXGI_FORMAT ToSRGB(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return f;
    }
}

static int64_t PickFormat(DXGI_FORMAT want, DXGI_FORMAT alt) {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_sess, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(g_sess, n, &n, fmts.data());
    for (auto f : fmts)
        if ((DXGI_FORMAT)f == want) return f;
    for (auto f : fmts)
        if ((DXGI_FORMAT)f == alt) return f;
    return 0;
}

static bool EnsureChain(Chain &c, UINT w, UINT h, DXGI_FORMAT key, DXGI_FORMAT scFmt) {
    if (c.sc && c.w == w && c.h == h && c.fmt == key) return true;
    if (c.sc) {
        xrDestroySwapchain(c.sc);
        c = Chain{};
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = scFmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(g_sess, &ci, &c.sc));
    uint32_t n = 0;
    xrEnumerateSwapchainImages(c.sc, 0, &n, nullptr);
    c.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(c.sc, n, &n, (XrSwapchainImageBaseHeader *)c.images.data());
    c.w = w;
    c.h = h;
    c.fmt = key;
    c.scFmt = scFmt;
    PVR_LOG("swapchain %ux%u fmt %d (source fmt %d), %u images", w, h, (int)scFmt, (int)key, n);
    return true;
}

static bool EnsureQuad(UINT w, UINT h, DXGI_FORMAT fmt) {
    int64_t pick = PickFormat(ToSRGB(fmt), fmt);
    if (!pick) {
        PVR_LOG("no XR swapchain format compatible with backbuffer format %d", (int)fmt);
        return false;
    }
    return EnsureChain(g_quad, w, h, fmt, (DXGI_FORMAT)pick);
}

static DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}
static bool IsFloat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT ||
           f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

void CaptureEye(int eye, ID3D11Texture2D *src) {
    if (eye < 0 || eye > 1 || !src) return;
    ID3D11Device *dev = GameDevice();
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    EyeImg &e = g_eyeImg[eye];
    if (!e.tex || e.desc.Width != d.Width || e.desc.Height != d.Height || e.desc.Format != d.Format) {
        if (e.srv) e.srv->Release();
        if (e.tex) e.tex->Release();
        e = EyeImg{};
        D3D11_TEXTURE2D_DESC c = d;
        c.SampleDesc = {1, 0};
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.MipLevels = 1;
        c.ArraySize = 1;
        if (FAILED(dev->CreateTexture2D(&c, nullptr, &e.tex))) {
            PVR_LOG("CaptureEye: texture create failed (fmt %d)", (int)d.Format);
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = Typed(d.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(e.tex, &sv, &e.srv);
        e.desc = d;
        PVR_LOG("eye %d capture target %ux%u fmt %d samples %u mips %u", eye, d.Width, d.Height, (int)d.Format,
                d.SampleDesc.Count, d.MipLevels);
    }
    ID3D11DeviceContext *ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (d.SampleDesc.Count > 1)
        ctx->ResolveSubresource(e.tex, 0, src, 0, Typed(d.Format));
    else if (d.MipLevels == 1 && d.ArraySize == 1)
        ctx->CopyResource(e.tex, src);
    else
        ctx->CopySubresourceRegion(e.tex, 0, 0, 0, 0, src, 0, nullptr);
    static int s_probe = 0;
    if (s_probe < 6 && e.desc.Format != DXGI_FORMAT_UNKNOWN) {
        // diagnostics: read back the centre pixel of the captured eye
        D3D11_TEXTURE2D_DESC sd = e.desc;
        sd.Width = sd.Height = 1;
        sd.MipLevels = sd.ArraySize = 1;
        sd.SampleDesc = {1, 0};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D *st = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
            D3D11_BOX box{e.desc.Width / 2, e.desc.Height / 2, 0, e.desc.Width / 2 + 1, e.desc.Height / 2 + 1, 1};
            ctx->CopySubresourceRegion(st, 0, 0, 0, 0, e.tex, 0, &box);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                const uint8_t *px = (const uint8_t *)m.pData;
                PVR_LOG("probe eye %d centre bytes %02X %02X %02X %02X %02X %02X %02X %02X", eye, px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
                ctx->Unmap(st, 0);
            }
            st->Release();
        }
        ++s_probe;
    }
    ctx->Release();
    std::lock_guard<std::mutex> lk(g_frameMtx);
    e.frame = g_frameIndex;
}

bool FrameViews(XrView out[2]) {
    std::lock_guard<std::mutex> lk(g_frameMtx);
    if (!g_running || !g_frameBegun || !g_viewsValid) return false;
    out[0] = g_views[0];
    out[1] = g_views[1];
    return true;
}

// Blit the captured eye into its XR swapchain. The swapchain is sRGB: 8-bit sources are
// already gamma encoded so they go through a UNORM view (raw); float sources through sRGB.
static bool SubmitEye(ID3D11DeviceContext *ctx, int eye, XrCompositionLayerProjectionView &pv) {
    EyeImg &e = g_eyeImg[eye];
    UINT w = e.desc.Width, h = e.desc.Height;
    DXGI_FORMAT scFmt = (DXGI_FORMAT)PickFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!scFmt || !EnsureChain(g_eyeChain[eye], w, h, e.desc.Format, scFmt)) return false;
    Chain &c = g_eyeChain[eye];
    if (g_eyeRtv[eye].size() != c.images.size()) {
        for (auto *r : g_eyeRtv[eye])
            if (r) r->Release();
        g_eyeRtv[eye].assign(c.images.size(), nullptr);
        bool linear = IsFloat(Typed(e.desc.Format));
        for (size_t i = 0; i < c.images.size(); ++i) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = linear ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            HRESULT hr = GameDevice()->CreateRenderTargetView(c.images[i].texture, &rd, &g_eyeRtv[eye][i]);
            if (FAILED(hr)) {
                PVR_LOG("eye RTV with fmt %d failed (0x%08X), using default view", (int)rd.Format, (unsigned)hr);
                GameDevice()->CreateRenderTargetView(c.images[i].texture, nullptr, &g_eyeRtv[eye][i]);
            }
        }
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(c.sc, &ai, &idx)) || XR_FAILED(xrWaitSwapchainImage(c.sc, &wi)))
        return false;
    blit::Blit(ctx, e.srv, g_eyeRtv[eye][idx], w, h);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(c.sc, &ri);
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = g_views[eye].pose;
    pv.fov = g_views[eye].fov;
    pv.subImage.swapchain = c.sc;
    pv.subImage.imageRect = {{0, 0}, {(int32_t)w, (int32_t)h}};
    return true;
}

static void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *s = (XrEventDataSessionStateChanged *)&ev;
            g_state = s->state;
            PVR_LOG("session state -> %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = XR_SUCCEEDED(xrBeginSession(g_sess, &bi));
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess);
                g_running = false;
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void BeginNextFrame() {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_sess, nullptr, &fs))) return;
    xrBeginFrame(g_sess, nullptr);

    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = g_local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    bool ok = XR_SUCCEEDED(xrLocateViews(g_sess, &li, &vs, 2, &n, v)) && n == 2 &&
              (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    // head yaw from the left eye orientation (forward = -Z), positive = turned left
    float headYaw = 0.0f;
    if (ok) {
        const XrQuaternionf &q = v[0].pose.orientation;
        float fx = -2.0f * (q.x * q.z + q.w * q.y), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        headYaw = std::atan2(-fx, -fz);
    }
    InputSync(g_sess, g_state == XR_SESSION_STATE_FOCUSED, headYaw);
    LocateHands(g_local, fs.predictedDisplayTime);
    std::lock_guard<std::mutex> lk(g_frameMtx);
    g_fs = fs;
    g_frameBegun = true;
    g_viewsValid = ok && fs.shouldRender;
    if (ok) g_views[0] = v[0], g_views[1] = v[1];
    ++g_frameIndex;
}

struct LayerChain {
    Chain chain;
    std::vector<ID3D11RenderTargetView *> rtv;
};
static LayerChain g_hudLayer, g_menuLayer, g_gaugeLayer;

// copy an overlay texture into its quad-layer swapchain
static bool CopyToChain(ID3D11DeviceContext *ctx, LayerChain &L, ID3D11ShaderResourceView *srv, UINT w, UINT h) {
    DXGI_FORMAT scFmt = (DXGI_FORMAT)PickFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!scFmt || !EnsureChain(L.chain, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, scFmt)) return false;
    if (L.rtv.size() != L.chain.images.size()) {
        for (auto *r : L.rtv)
            if (r) r->Release();
        L.rtv.assign(L.chain.images.size(), nullptr);
        for (size_t i = 0; i < L.chain.images.size(); ++i) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // raw: overlay colours are already gamma encoded
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(GameDevice()->CreateRenderTargetView(L.chain.images[i].texture, &rd, &L.rtv[i])))
                GameDevice()->CreateRenderTargetView(L.chain.images[i].texture, nullptr, &L.rtv[i]);
        }
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(L.chain.sc, &ai, &idx)) || XR_FAILED(xrWaitSwapchainImage(L.chain.sc, &wi)))
        return false;
    blit::Blit(ctx, srv, L.rtv[idx], w, h, true);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(L.chain.sc, &ri);
    return true;
}

static XrCompositionLayerQuad MakeQuad(const LayerChain &L, XrRect2Di rect, XrExtent2Df size, const XrPosef &pose) {
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    q.space = g_local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = L.chain.sc;
    q.subImage.imageRect = rect;
    q.size = size;
    q.pose = pose;
    return q;
}

// Head-locked quads in LOCAL space at this frame's predicted head pose (the same views the
// projection layer uses); a VIEW-space quad behaved world-fixed on SteamVR.
//  HUD canvas (laid out for aspect Cfg().hudAspect): top part [0, Split) on a panel whose canvas
//  centre sits at the gaze centre, bottom part (status bar) on a panel tilted BarPitch below.
//  Menu layer: square panel centred on the gaze.
static int SubmitHud(ID3D11DeviceContext *ctx, const overlay::Out &hud, const overlay::Out &menu,
                     XrCompositionLayerQuad out[6]) {
    int n = 0;
    const float d2r = 0.017453293f;
    XrPosef head = vm::HeadPose(g_views[0].pose, g_views[1].pose);
    // body yaw for the chest panel: follows the head lazily
    static bool s_have = false;
    static float s_yaw = 0;
    float hy = vm::YawOf(head.orientation);
    s_yaw = s_have ? vm::FollowYaw(s_yaw, hy, Cfg().gaugeDeadzone * d2r) : hy;
    s_have = true;
    XrPosef chest = vm::ChestPose(head.position, s_yaw, Cfg().gaugeDown, Cfg().gaugeForward, Cfg().gaugeTilt * d2r);
    if (hud.srv && CopyToChain(ctx, g_hudLayer, hud.srv, hud.w, hud.h)) {
        float aspect = Cfg().hudAspect > 0 ? Cfg().hudAspect : (float)hud.w / (float)hud.h;
        float split = std::clamp(Cfg().hudSplit, 0.5f, 1.0f);
        int32_t splitPx = (int32_t)(split * hud.h);
        float W = Cfg().hudWidth, H = W / aspect;
        // messages: their band of the canvas, head-locked, MsgPitch degrees below the gaze
        float m0 = std::clamp(Cfg().msgY0, 0.0f, 1.0f), m1 = std::clamp(Cfg().msgY1, m0 + 0.01f, 1.0f);
        out[n++] = MakeQuad(g_hudLayer, {{0, (int32_t)(m0 * hud.h)}, {(int32_t)hud.w, (int32_t)((m1 - m0) * hud.h)}},
                            {W, H * (m1 - m0)}, vm::PanelPose(head, Cfg().hudDistance, Cfg().msgPitch * d2r, 0.0f));
        // power/compass icon (top-right of the canvas) on the chest panel, right of the bars
        if (Cfg().gauges && Cfg().iconSize > 0) {
            const float *ic = Cfg().iconSrc;
            int32_t x0 = (int32_t)(ic[0] * hud.w), y0 = (int32_t)(ic[2] * hud.h);
            int32_t iw = (int32_t)((ic[1] - ic[0]) * hud.w), ih = (int32_t)((ic[3] - ic[2]) * hud.h);
            if (iw > 0 && ih > 0) {
                float s = Cfg().iconSize;
                XrPosef off{{0, 0, 0, 1}, {Cfg().gaugeWidth * 0.5f + 0.01f + s * 0.5f, 0, 0}};
                out[n++] = MakeQuad(g_hudLayer, {{x0, y0}, {iw, ih}}, {s, s * (float)ih / (float)iw}, vm::Compose(chest, off));
            }
        }
        if (splitPx < (int32_t)hud.h && Cfg().barWidth > 0) {
            float bw = Cfg().barWidth;
            out[n++] = MakeQuad(g_hudLayer, {{0, splitPx}, {(int32_t)hud.w, (int32_t)hud.h - splitPx}},
                                {bw, bw / aspect * (1.0f - split)},
                                vm::PanelPose(head, Cfg().barDistance, Cfg().barPitch * 0.017453293f, 0.0f));
        }
    }
    if (menu.srv && CopyToChain(ctx, g_menuLayer, menu.srv, menu.w, menu.h)) {
        float W = Cfg().menuWidth;
        out[n++] = MakeQuad(g_menuLayer, {{0, 0}, {(int32_t)menu.w, (int32_t)menu.h}}, {W, W * (float)menu.h / (float)menu.w},
                            vm::PanelPose(head, Cfg().hudDistance, 0.0f, -Cfg().hudDown));
    }
    // chest panel (health / ammo / air): below the head along a lazily-following body yaw, tilted up
    hudstate::Stats st;
    if (Cfg().gauges && hudstate::Get(st)) {
        UINT gw = 0, gh = 0;
        if (ID3D11ShaderResourceView *gs = gauges::Render(ctx, st, hud.srv, gw, gh)) {
            // chain sized for the full panel texture; the quad shows the used rows
            if (CopyToChain(ctx, g_gaugeLayer, gs, 600, 512)) {
                float W = Cfg().gaugeWidth;
                out[n++] = MakeQuad(g_gaugeLayer, {{0, 0}, {(int32_t)gw, (int32_t)gh}}, {W, W * (float)gh / (float)gw}, chest);
            }
        }
    }
    static uint64_t s_n = 0;
    if (n && (s_n++ % 900) == 0)
        PVR_LOG("hud quads: %d (hud %ux%u menu %ux%u) head (%.2f %.2f %.2f)", n, hud.w, hud.h, menu.w, menu.h,
                head.position.x, head.position.y, head.position.z);
    return n;
}

// FakeXR harness: no OpenXR; the script provides the views for the next frame.
static bool g_menuUp = false;  // menu layer drawn this frame
static void FakePresent(ID3D11ShaderResourceView *hud) {
    {  // whole-frame render-target trace, as in the real path
        static uint64_t s_n = 0;
        static int s_logs = 0;
        bool log = (s_n % 300) == 150 && s_logs < 8;
        if (log) ++s_logs, PVR_LOG("frame trace (fake): overlay %p remaps scene %p", (void *)overlay::Target(), (void *)overlay::SceneTarget());
        TraceEnd(1, log);
        TraceBegin(1, nullptr);
        ++s_n;
    }
    {  // exercise the wrist gauges (shader compile + values) like the real path does
        hudstate::Stats st;
        UINT gw = 0, gh = 0;
        ID3D11DeviceContext *c = nullptr;
        GameDevice()->GetImmediateContext(&c);
        if (Cfg().gauges && hudstate::Get(st) && gauges::Render(c, st, hud, gw, gh)) {
            static int s_n = 0;
            if (s_n == 0) PVR_LOG("fake: gauges rendered %ux%u", gw, gh);
            if (++s_n == 600) gauges::Dump(c, (GameDir() + L"pvr_gauges.raw").c_str()), PVR_LOG("fake: gauges dumped");
        }
        c->Release();
    }
    XrView v[2];
    bool have = g_eyeImg[0].frame == g_frameIndex && g_eyeImg[1].frame == g_frameIndex;
    PadSetMenuMode(!have || g_menuUp);
    fake::Frame(hud, have ? g_eyeImg[0].tex : nullptr, have ? g_eyeImg[1].tex : nullptr, v);
    std::lock_guard<std::mutex> lk(g_frameMtx);
    g_running = true;
    g_frameBegun = true;
    g_viewsValid = true;
    g_views[0] = v[0], g_views[1] = v[1];
    ++g_frameIndex;
}

void OnPresent(IDXGISwapChain *sc) {
    overlay::Out hudOut, menuOut;
    overlay::End(hudOut, menuOut);
    ID3D11ShaderResourceView *hudSrv = hudOut.srv ? hudOut.srv : menuOut.srv;
    if (Cfg().fakeXR) {
        g_menuUp = menuOut.srv != nullptr;
        if (GameDevice()) FakePresent(hudSrv);
        return;
    }
    if (g_failed) return;
    ID3D11Device *dev = GameDevice();
    if (!dev) return;
    if (!g_inst) {
        if (!InitInstance(dev)) {
            g_failed = true;
            PVR_LOG("OpenXR init failed; running flat");
            return;
        }
    }
    PollEvents();
    if (!g_running) {
        std::lock_guard<std::mutex> lk(g_frameMtx);
        g_frameBegun = false;
        return;
    }
    if (!g_frameBegun) {
        BeginNextFrame();  // first frame: nothing rendered for it yet
        return;
    }

    XrFrameState fs;
    uint64_t frame;
    bool viewsValid;
    {
        std::lock_guard<std::mutex> lk(g_frameMtx);
        fs = g_fs;
        frame = g_frameIndex;
        viewsValid = g_viewsValid;
    }
    // stopgap: L-grip + View toggles the flat-screen view (menus drawn over a 3D scene)
    {
        static bool s_prev = false, s_flat = false;
        PadState ps = PadSnapshot();
        bool combo = ps.buttons[PB_PADDLE2] && ps.buttons[PB_BACK];
        if (combo && !s_prev) {
            s_flat = !s_flat;
            PVR_LOG("flat view %s", s_flat ? "on" : "off");
        }
        s_prev = combo;
        g_forceFlat = s_flat;
    }
    bool worldDrawn = viewsValid && g_eyeImg[0].frame == frame && g_eyeImg[1].frame == frame;
    bool stereo = !g_forceFlat && worldDrawn;
    // a menu is up when no 3D world was drawn (title, loading, level map) or the menu layer was (pause menu)
    PadSetMenuMode(!worldDrawn || menuOut.srv != nullptr);
    // whole-frame render-target trace (diagnostics for HUD/menu compositing)
    {
        static uint64_t s_n = 0;
        static int s_logs = 0;
        bool log = (s_n % 300) == 150 && s_logs < 12;
        if (log) {
            ++s_logs;
            ID3D11Texture2D *bbt = nullptr;
            sc->GetBuffer(0, IID_PPV_ARGS(&bbt));
            PVR_LOG("frame trace (stereo=%d) backbuffer %p:", (int)stereo, (void *)bbt);
            if (bbt) bbt->Release();
        }
        TraceEnd(1, log);
        TraceBegin(1, nullptr);
        ++s_n;
    }
    static uint64_t s_frames = 0;
    if ((s_frames++ % 600) == 0)
        PVR_LOG("frame %llu: state=%d shouldRender=%d stereo=%d hud=%d flat=%d", (unsigned long long)s_frames, (int)g_state,
                (int)fs.shouldRender, (int)stereo, (int)(hudSrv != nullptr), (int)g_forceFlat);

    ID3D11DeviceContext *ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pviews[2];
    XrCompositionLayerQuad hudQuads[6];
    const XrCompositionLayerBaseHeader *layers[8];
    uint32_t nLayers = 0;

    if (fs.shouldRender && stereo && SubmitEye(ctx, 0, pviews[0]) && SubmitEye(ctx, 1, pviews[1])) {
        proj.space = g_local;
        proj.viewCount = 2;
        proj.views = pviews;
        layers[nLayers++] = (XrCompositionLayerBaseHeader *)&proj;
        int nq = SubmitHud(ctx, hudOut, menuOut, hudQuads);
        for (int i = 0; i < nq; ++i) layers[nLayers++] = (XrCompositionLayerBaseHeader *)&hudQuads[i];
    } else if (fs.shouldRender) {
        ID3D11Texture2D *bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            D3D11_TEXTURE2D_DESC d;
            bb->GetDesc(&d);
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            if (EnsureQuad(d.Width, d.Height, d.Format) &&
                XR_SUCCEEDED(xrAcquireSwapchainImage(g_quad.sc, &ai, &idx)) &&
                XR_SUCCEEDED(xrWaitSwapchainImage(g_quad.sc, &wi))) {
                ID3D11Texture2D *dst = g_quad.images[idx].texture;
                if (d.SampleDesc.Count > 1) {
                    if (!g_resolve) {
                        D3D11_TEXTURE2D_DESC rd = d;
                        rd.SampleDesc = {1, 0};
                        rd.BindFlags = 0;
                        rd.MiscFlags = 0;
                        dev->CreateTexture2D(&rd, nullptr, &g_resolve);
                    }
                    ctx->ResolveSubresource(g_resolve, 0, bb, 0, d.Format);
                    ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, g_resolve, 0, nullptr);
                } else {
                    ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, bb, 0, nullptr);
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_quad.sc, &ri);
                float wM = Cfg().screenWidth;
                quad.space = g_local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g_quad.sc;
                quad.subImage.imageRect = {{0, 0}, {(int32_t)d.Width, (int32_t)d.Height}};
                quad.pose.orientation.w = 1.0f;
                quad.pose.position = {0.0f, 0.0f, -Cfg().screenDistance};
                quad.size = {wM, wM * 0.75f};  // the game's 2D layer is 4:3
                layers[nLayers++] = (XrCompositionLayerBaseHeader *)&quad;
            }
            bb->Release();
        }
    }
    ctx->Release();

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nLayers;
    ei.layers = layers;
    XrResult r = xrEndFrame(g_sess, &ei);
    static int s_errs = 0;
    if (XR_FAILED(r) && s_errs++ < 10) PVR_LOG("xrEndFrame failed %d", (int)r);
    {
        std::lock_guard<std::mutex> lk(g_frameMtx);
        g_frameBegun = false;
    }
    BeginNextFrame();
}

}  // namespace pvr::xr
