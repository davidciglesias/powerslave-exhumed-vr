// First-person weapon in the hand. The game draws its 2D weapon sprite (with all animations,
// muzzle flashes, ammo belts...) via FUN_14020fc20 inside the world pass. During the first
// eye we redirect that draw into our own transparent render target, and skip it for the
// second eye. Then in each eye we draw that image as a card attached to the right controller's
// aim pose: a cropped region of the screen (where the game places the weapon) facing the
// player, depth-tested against the world.
#include "render/hand_weapon.h"

#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <windows.h>
#include <d3dcompiler.h>
#include <MinHook.h>

#include <cmath>
#include <cstring>

#include "common/config.h"
#include "game/hud_state.h"
#include "common/log.h"
#include "game/game_addr.h"
#include "hooks/d3d_hooks.h"
#include "render/stereo.h"
#include "xr/xr_input.h"

namespace pvr::hand {
using namespace pvr::game;

using PFN_WeaponDraw = void(__fastcall *)(void *weapon);
static PFN_WeaponDraw o_WeaponDraw = nullptr;
static int g_eye = -1;
void SetEye(int eye) { g_eye = eye; }

// captured weapon image
static ID3D11Texture2D *g_tex = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;
static ID3D11ShaderResourceView *g_srv = nullptr;
static UINT g_w = 0, g_h = 0;
static bool g_haveImage = false;

static bool EnsureTarget(UINT w, UINT h) {
    if (g_tex && g_w == w && g_h == h) return true;
    if (g_srv) g_srv->Release(), g_srv = nullptr;
    if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
    if (g_tex) g_tex->Release(), g_tex = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device *dev = GameDevice();
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_tex))) return false;
    dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv);
    dev->CreateShaderResourceView(g_tex, nullptr, &g_srv);
    g_w = w;
    g_h = h;
    PVR_LOG("hand: weapon target %ux%u", w, h);
    return g_rtv && g_srv;
}

// diagnostics ([Debug] DumpWeapons=1): each weapon's 2D image, once, after it was held 90 frames
static void MaybeDumpWeapon(ID3D11DeviceContext *ctx) {
    if (!Cfg().dumpWeapons) return;
    static int s_last = -1, s_frames = 0;
    static bool s_done[16] = {};
    hudstate::Stats st;
    if (!hudstate::Get(st) || st.weapon < 0 || st.weapon >= 16) return;
    if (st.weapon != s_last) s_last = st.weapon, s_frames = 0;
    if (s_done[st.weapon] || ++s_frames < 90) return;
    s_done[st.weapon] = true;
    D3D11_TEXTURE2D_DESC d;
    g_tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st2 = nullptr;
    if (FAILED(GameDevice()->CreateTexture2D(&d, nullptr, &st2))) return;
    ctx->CopyResource(st2, g_tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st2, 0, D3D11_MAP_READ, 0, &m))) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%spvr_weapon_%d.raw", GameDir().c_str(), st.weapon);
        FILE *f = _wfopen(path, L"wb");
        if (f) {
            for (UINT y = 0; y < d.Height; ++y) fwrite((uint8_t *)m.pData + y * m.RowPitch, 4, d.Width, f);
            fclose(f);
        }
        ctx->Unmap(st2, 0);
        PVR_LOG("hand: dumped weapon %d image %ux%u", st.weapon, d.Width, d.Height);
    }
    st2->Release();
}

// ---- left hand: cut out of a weapon's 2D image once (default: the ring's left hand), brightness
// normalised, saved to <game>\PowerslaveVR_hand.raw and loaded from there in later sessions ----
static ID3D11Texture2D *g_handTex = nullptr;
static ID3D11ShaderResourceView *g_handSrv = nullptr;
static UINT g_handW = 0, g_handH = 0;  // crop size in pixels of the 2048 weapon image
static float g_handGain = 1.0f;
static bool g_handTried = false;
static std::wstring HandFile() { return GameDir() + L"PowerslaveVR_hand.raw"; }

static bool MakeHandTex(UINT w, UINT h, const void *px) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px, w * 4, 0};
    if (FAILED(GameDevice()->CreateTexture2D(&d, px ? &sd : nullptr, &g_handTex))) return false;
    GameDevice()->CreateShaderResourceView(g_handTex, nullptr, &g_handSrv);
    g_handW = w, g_handH = h;
    return g_handSrv != nullptr;
}

static void LoadHand() {
    if (g_handTried) return;
    g_handTried = true;
    FILE *f = _wfopen(HandFile().c_str(), L"rb");
    if (!f) return;
    uint32_t hdr[4] = {};
    std::vector<uint8_t> px;
    if (fread(hdr, 4, 4, f) == 4 && hdr[0] == 0x48525650 /* PVRH */ && hdr[1] && hdr[2] && hdr[1] <= 4096 && hdr[2] <= 4096) {
        px.resize((size_t)hdr[1] * hdr[2] * 4);
        if (fread(px.data(), 1, px.size(), f) == px.size()) {
            float gain;
            memcpy(&gain, &hdr[3], 4);
            if (MakeHandTex(hdr[1], hdr[2], px.data())) g_handGain = gain, PVR_LOG("hand: loaded %ux%u gain %.2f", hdr[1], hdr[2], gain);
        }
    }
    fclose(f);
}

// after the weapon image was drawn: if the source weapon has been held a while, cut the hand out
static void MaybeCaptureHand(ID3D11DeviceContext *ctx) {
    const Config &c = Cfg();
    if (!c.handEnable || g_handTex) return;
    static int s_frames = 0;
    hudstate::Stats st;
    if (!hudstate::Get(st) || st.weapon != c.handSourceWeapon) { s_frames = 0; return; }
    if (++s_frames < 90) return;
    D3D11_TEXTURE2D_DESC wd;
    g_tex->GetDesc(&wd);
    UINT x0 = (UINT)(c.handSrc[0] * wd.Width), x1 = (UINT)(c.handSrc[1] * wd.Width);
    UINT y0 = (UINT)(c.handSrc[2] * wd.Height), y1 = (UINT)(c.handSrc[3] * wd.Height);
    if (x1 <= x0 || y1 <= y0 || x1 > wd.Width || y1 > wd.Height) return;
    UINT w = x1 - x0, h = y1 - y0;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = wd.Format;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stg = nullptr;
    if (FAILED(GameDevice()->CreateTexture2D(&sd, nullptr, &stg))) return;
    D3D11_BOX box{x0, y0, 0, x1, y1, 1};
    ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, g_tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    std::vector<uint8_t> px((size_t)w * h * 4);
    bool ok = SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        for (UINT y = 0; y < h; ++y) memcpy(&px[(size_t)y * w * 4], (uint8_t *)m.pData + y * m.RowPitch, w * 4);
        ctx->Unmap(stg, 0);
    }
    stg->Release();
    if (!ok) return;
    // brightness: the sprite carries the sector light it was drawn in; scale the 95th percentile
    // luminance of the opaque pixels to ~0.85
    std::vector<int> lum;
    for (size_t i = 0; i < px.size(); i += 4)
        if (px[i + 3] >= 128) lum.push_back((px[i] * 3 + px[i + 1] * 6 + px[i + 2]) / 10);
    if (lum.size() < 100) return;  // nothing there (e.g. mid-switch animation)
    std::nth_element(lum.begin(), lum.begin() + lum.size() * 95 / 100, lum.end());
    float p95 = (float)lum[lum.size() * 95 / 100] / 255.0f;
    float gain = std::clamp(0.85f / (std::max)(p95, 0.02f), 1.0f, 6.0f);
    if (!MakeHandTex(w, h, px.data())) return;
    g_handGain = gain;
    FILE *f = _wfopen(HandFile().c_str(), L"wb");
    if (f) {
        uint32_t hdr[4] = {0x48525650, w, h, 0};
        memcpy(&hdr[3], &gain, 4);
        fwrite(hdr, 4, 4, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
    }
    PVR_LOG("hand: captured from weapon %d, %ux%u, p95 luma %.2f -> gain %.2f, saved", st.weapon, w, h, p95, gain);
}

static void __fastcall hk_WeaponDraw(void *weapon) {
    if (g_eye < 0 || !Cfg().handWeapon) return o_WeaponDraw(weapon);
    if (g_eye == 1) return;  // already captured during eye 0; nothing on the HUD for eye 1
    ID3D11DeviceContext *ctx = nullptr;
    GameDevice()->GetImmediateContext(&ctx);
    ID3D11RenderTargetView *prevRtv = nullptr;
    ID3D11DepthStencilView *prevDsv = nullptr;
    ctx->OMGetRenderTargets(1, &prevRtv, &prevDsv);
    UINT w = 0, h = 0;
    if (prevRtv) {
        ID3D11Resource *r = nullptr;
        prevRtv->GetResource(&r);
        ID3D11Texture2D *t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t)))) {
            D3D11_TEXTURE2D_DESC d;
            t->GetDesc(&d);
            w = d.Width, h = d.Height;
            t->Release();
        }
        if (r) r->Release();
    }
    if (w && EnsureTarget(w, h)) {
        const float clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(g_rtv, clear);
        SetRenderTargetRedirect(g_rtv);
        ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        o_WeaponDraw(weapon);
        SetRenderTargetRedirect(nullptr);
        ctx->OMSetRenderTargets(1, &prevRtv, prevDsv);
        g_haveImage = true;
        MaybeDumpWeapon(ctx);
        MaybeCaptureHand(ctx);
    } else {
        o_WeaponDraw(weapon);
    }
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
    ctx->Release();
}

// ---- card rendering ----
static const char *kCardShader = R"(
// Voxel extrusion of the weapon sprite. The box [0,1]^3 (local s along right, t along up, d along
// the aim, d = 0 towards the player) is rasterized; each pixel traces the camera ray through a 2D
// grid of sprite texels (2D DDA); opaque texels are solid columns spanning d in [0,1].
cbuffer PVRCard : register(b13) {
    float4 m0; float4 m1; float4 m2; float4 m3;  // world -> clip rows
    float4 O;    // box origin (bottom-left-front corner), world
    float4 RW;   // right * width
    float4 UH;   // up * height
    float4 FD;   // aim * depth
    float4 cam;  // eye position, world
    float4 uvr;  // crop uv0 (xy) .. uv1 (zw), v = 0 at the top
    float4 grid; // voxels x, voxels y, side shade, max steps
    float4 extra; // x colour gain, y mirror (1 = flip horizontally)
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float3 wp : TEXCOORD0; };
static const uint kIdx[36] = {0,1,2, 2,1,3, 4,6,5, 5,6,7, 0,2,4, 4,2,6, 1,5,3, 3,5,7, 0,4,1, 1,4,5, 2,3,6, 6,3,7};
float4 Clip(float3 p) { float4 h = float4(p, 1.0); return float4(dot(h, m0), dot(h, m1), dot(h, m2), dot(h, m3)); }
V vs(uint id : SV_VertexID) {
    uint c = kIdx[id];
    float3 p = O.xyz + RW.xyz * (c & 1) + UH.xyz * ((c >> 1) & 1) + FD.xyz * ((c >> 2) & 1);
    V o;
    o.pos = Clip(p);
    o.wp = p;
    return o;
}
float3 ToLocal(float3 p) {
    float3 d = p - O.xyz;
    return float3(dot(d, RW.xyz) / dot(RW.xyz, RW.xyz), dot(d, UH.xyz) / dot(UH.xyz, UH.xyz), dot(d, FD.xyz) / dot(FD.xyz, FD.xyz));
}
float4 Texel(int2 c) {
    float2 f = float2((c.x + 0.5) / grid.x, 1.0 - (c.y + 0.5) / grid.y);
    if (extra.y > 0.5) f.x = 1.0 - f.x;
    float4 s = tex.SampleLevel(smp, lerp(uvr.xy, uvr.zw, f), 0);
    s.rgb = min(1.0, s.rgb * extra.x);
    return s;
}
struct PO { float4 col : SV_Target; float depth : SV_Depth; };
PO ps(V i) {
    float3 ro = ToLocal(cam.xyz);
    float3 rd = ToLocal(i.wp) - ro;  // t = 1 at this fragment
    float3 inv = 1.0 / (abs(rd) > 1e-8 ? rd : 1e-8);
    float3 ta = (0.0 - ro) * inv, tb = (1.0 - ro) * inv;
    float3 tn = min(ta, tb), tf = max(ta, tb);
    float t0 = max(max(tn.x, tn.y), tn.z), t1 = min(min(tf.x, tf.y), tf.z);
    if (t1 < max(t0, 0.0)) discard;
    int axis = tn.x >= tn.y ? (tn.x >= tn.z ? 0 : 2) : (tn.y >= tn.z ? 1 : 2);
    if (t0 < 0.0) { t0 = 0.0; axis = 3; }  // camera inside the box
    float2 N = grid.xy;
    float3 p = ro + rd * t0;
    int2 cell = clamp(int2(floor(p.xy * N)), int2(0, 0), int2(N) - 1);
    int2 stp = int2(rd.x > 0 ? 1 : -1, rd.y > 0 ? 1 : -1);
    float2 tDelta = abs(inv.xy / N);
    float2 tMax = ((float2(cell + (stp > 0 ? 1 : 0)) / N) - ro.xy) * inv.xy;
    float t = t0;
    [loop] for (int k = 0; k < (int)grid.w; ++k) {
        float4 s = Texel(cell);
        if (s.a >= 0.5) {
            float shade = 1.0;
            if (axis == 0) shade = grid.z;                                                  // side walls
            else if (axis == 1) shade = rd.y < 0 ? lerp(grid.z, 1.0, 0.6) : grid.z * 0.8;  // top lit, bottom dark
            else if (axis == 2 && rd.z < 0) shade = grid.z * 0.9;                           // back face
            float3 hit = cam.xyz + (i.wp - cam.xyz) * t;
            float4 h = Clip(hit);
            PO o;
            o.col = float4(s.rgb * shade, 1.0);
            o.depth = h.z / h.w;
            return o;
        }
        if (tMax.x < tMax.y) { t = tMax.x; tMax.x += tDelta.x; cell.x += stp.x; axis = 0; }
        else { t = tMax.y; tMax.y += tDelta.y; cell.y += stp.y; axis = 1; }
        if (t > t1 || any(cell < 0) || any(cell >= int2(N))) break;
    }
    discard;
    PO o = (PO)0;
    return o;
}
)";
static ID3D11VertexShader *g_vs = nullptr;
static ID3D11PixelShader *g_ps = nullptr;
static ID3D11Buffer *g_cb = nullptr;
static ID3D11SamplerState *g_samp = nullptr;
static ID3D11RasterizerState *g_rs = nullptr;
static ID3D11DepthStencilState *g_dsLE = nullptr, *g_dsGE = nullptr;
static ID3D11BlendState *g_bs = nullptr;
static bool g_initFailed = false;

static bool InitCard() {
    if (g_vs) return true;
    if (g_initFailed) return false;
    ID3D11Device *dev = GameDevice();
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kCardShader, strlen(kCardShader), "pvr_card", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(kCardShader, strlen(kCardShader), "pvr_card", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, &err))) {
        PVR_LOG("hand: shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        g_initFailed = true;
        return false;
    }
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_ps);
    vb->Release();
    pb->Release();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16 * 12;  // m0-m3, O, RW, UH, FD, cam, uvr, grid, extra
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // crisp PS1 pixels
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_samp);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dd, &g_dsLE);
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    dev->CreateDepthStencilState(&dd, &g_dsGE);
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &g_bs);
    return g_vs && g_ps && g_cb;
}

static void QuatAxes(const XrQuaternionf &q, float right[3], float up[3], float fwd[3]) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    right[0] = 1 - 2 * (y * y + z * z), right[1] = 2 * (x * y + z * w), right[2] = 2 * (x * z - y * w);
    up[0] = 2 * (x * y - z * w), up[1] = 1 - 2 * (x * x + z * z), up[2] = 2 * (y * z + x * w);
    fwd[0] = -2 * (x * z + y * w), fwd[1] = -2 * (y * z - x * w), fwd[2] = -(1 - 2 * (x * x + y * y));
}

struct Sprite {
    ID3D11ShaderResourceView *srv;
    float u0, v0, u1, v1;  // crop in the texture
    float cropW, cropH;    // crop size as a fraction of the 2048 game screen (4:3 virtual layout)
    float pxW, pxH;        // crop size in screen pixels (voxel grid)
    int pose;              // xr hand pose: 0 left aim, 1 right aim
    float width, forward, up, depth, voxel, gain;
    bool mirror;
};

// Fill the constant buffer for one sprite; false if its pose is unavailable.
static bool SpriteConstants(const Sprite &sp, const float VP[16], const float camPos[3], const float base[4][3], float cbd[48]) {
    XrPosef pose;
    if (!xr::HandPose(sp.pose, pose)) return false;
    const float sc = Cfg().worldScale;
    auto toWorldDir = [&](const float v[3], float o[3]) {
        for (int i = 0; i < 3; ++i) o[i] = v[0] * base[0][i] + v[1] * base[1][i] + v[2] * base[2][i];
    };
    float r[3], u[3], f[3], R[3], U[3], F[3], P[3];
    QuatAxes(pose.orientation, r, u, f);
    toWorldDir(r, R);
    toWorldDir(u, U);
    toWorldDir(f, F);
    float p[3] = {pose.position.x * sc, pose.position.y * sc, pose.position.z * sc};
    toWorldDir(p, P);
    for (int i = 0; i < 3; ++i) P[i] += base[3][i];
    // the 2D layer is a 4:3 virtual screen stretched over the target: crop aspect in that space
    float aspect = (sp.cropW * 4.0f) / (sp.cropH * 3.0f);
    float W = sp.width * sc, H = W / aspect, D = (std::max)(sp.depth, 0.001f) * sc;
    // bottom-centre at pose + forward/up offsets, extending upward, extruded along the aim
    float bc[3];
    for (int i = 0; i < 3; ++i) bc[i] = P[i] + F[i] * (sp.forward * sc) + U[i] * (sp.up * sc);
    float vox = (std::max)(sp.voxel, 1.0f);
    float nx = (std::max)(1.0f, std::floor(sp.pxW / vox + 0.5f));
    float ny = (std::max)(1.0f, std::floor(sp.pxH / vox + 0.5f));
    memset(cbd, 0, 48 * sizeof(float));
    for (int row = 0; row < 4; ++row)
        for (int k = 0; k < 4; ++k) cbd[row * 4 + k] = VP[k * 4 + row];
    for (int i = 0; i < 3; ++i) {
        cbd[16 + i] = bc[i] - R[i] * W * 0.5f;  // O
        cbd[20 + i] = R[i] * W;
        cbd[24 + i] = U[i] * H;
        cbd[28 + i] = F[i] * D;
        cbd[32 + i] = camPos[i];
    }
    cbd[36] = sp.u0, cbd[37] = sp.v0, cbd[38] = sp.u1, cbd[39] = sp.v1;
    cbd[40] = nx, cbd[41] = ny, cbd[42] = Cfg().weaponEdgeShade, cbd[43] = nx + ny + 4;
    cbd[44] = sp.gain, cbd[45] = sp.mirror ? 1.0f : 0.0f;
    return true;
}

void DrawCard(ID3D11DeviceContext *ctx, const float VP[16], const float camPos[3], ID3D11Texture2D *sceneRT,
              ID3D11DepthStencilView *dsv, bool reversedZ) {
    const Config &c = Cfg();
    if (!sceneRT || !InitCard()) return;
    if (c.handEnable) LoadHand();
    Sprite list[2];
    int n = 0;
    if (c.handWeapon && g_haveImage) {
        D3D11_TEXTURE2D_DESC wd;
        g_tex->GetDesc(&wd);
        float u0 = c.weaponCropX0, u1 = c.weaponCropX1, v0 = c.weaponCropY0, v1 = c.weaponCropY1;
        list[n++] = {g_srv, u0, v0, u1, v1, u1 - u0, v1 - v0, (u1 - u0) * wd.Width, (v1 - v0) * wd.Height, 1,
                     c.weaponWidth, c.weaponForward, c.weaponUp, c.weaponThickness, c.weaponVoxel, 1.0f, false};
    }
    if (c.handEnable && g_handSrv)
        list[n++] = {g_handSrv, 0, 0, 1, 1, g_handW / 2048.0f, g_handH / 2048.0f, (float)g_handW, (float)g_handH, 0,
                     c.handWidth, c.handForward, c.handUp, c.handThickness, c.weaponVoxel, g_handGain, c.handMirror};
    if (!n) return;
    float base[4][3];
    if (!stereo::BaseFrame(base)) return;

    // render target view on the scene target (cached)
    static ID3D11Texture2D *s_rtTex = nullptr;
    static ID3D11RenderTargetView *s_sceneRtv = nullptr;
    if (s_rtTex != sceneRT) {
        if (s_sceneRtv) s_sceneRtv->Release(), s_sceneRtv = nullptr;
        GameDevice()->CreateRenderTargetView(sceneRT, nullptr, &s_sceneRtv);
        s_rtTex = sceneRT;
    }
    if (!s_sceneRtv) return;
    D3D11_TEXTURE2D_DESC td;
    sceneRT->GetDesc(&td);

    // save state we touch
    ID3D11RenderTargetView *oRtv = nullptr;
    ID3D11DepthStencilView *oDsv = nullptr;
    ctx->OMGetRenderTargets(1, &oRtv, &oDsv);
    ID3D11VertexShader *oVs = nullptr;
    ID3D11PixelShader *oPs = nullptr;
    ctx->VSGetShader(&oVs, nullptr, nullptr);
    ctx->PSGetShader(&oPs, nullptr, nullptr);
    ID3D11Buffer *oVcb = nullptr, *oPcb = nullptr;
    ctx->VSGetConstantBuffers(13, 1, &oVcb);
    ctx->PSGetConstantBuffers(13, 1, &oPcb);
    ID3D11ShaderResourceView *oSrv = nullptr;
    ctx->PSGetShaderResources(0, 1, &oSrv);
    ID3D11SamplerState *oSmp = nullptr;
    ctx->PSGetSamplers(0, 1, &oSmp);
    ID3D11RasterizerState *oRs = nullptr;
    ctx->RSGetState(&oRs);
    UINT nvp = 1;
    D3D11_VIEWPORT oVp{};
    ctx->RSGetViewports(&nvp, &oVp);
    ID3D11DepthStencilState *oDs = nullptr;
    UINT oRef = 0;
    ctx->OMGetDepthStencilState(&oDs, &oRef);
    ID3D11BlendState *oBs = nullptr;
    float oBf[4];
    UINT oMask = 0;
    ctx->OMGetBlendState(&oBs, oBf, &oMask);
    ID3D11InputLayout *oIl = nullptr;
    ctx->IAGetInputLayout(&oIl);
    D3D11_PRIMITIVE_TOPOLOGY oTopo;
    ctx->IAGetPrimitiveTopology(&oTopo);
    ID3D11GeometryShader *oGs = nullptr;
    ctx->GSGetShader(&oGs, nullptr, nullptr);

    D3D11_VIEWPORT vp{0, 0, (float)td.Width, (float)td.Height, 0, 1};
    ctx->OMSetRenderTargets(1, &s_sceneRtv, dsv);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs);
    ctx->OMSetDepthStencilState(reversedZ ? g_dsGE : g_dsLE, 0);
    ctx->OMSetBlendState(g_bs, nullptr, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->VSSetConstantBuffers(13, 1, &g_cb);
    ctx->PSSetConstantBuffers(13, 1, &g_cb);
    ctx->PSSetSamplers(0, 1, &g_samp);
    for (int i = 0; i < n; ++i) {
        float cbd[48];
        if (!SpriteConstants(list[i], VP, camPos, base, cbd)) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        memcpy(m.pData, cbd, sizeof(cbd));
        ctx->Unmap(g_cb, 0);
        ctx->PSSetShaderResources(0, 1, &list[i].srv);
        ctx->Draw(36, 0);
    }

    ID3D11ShaderResourceView *nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetRenderTargets(1, &oRtv, oDsv);
    ctx->VSSetShader(oVs, nullptr, 0);
    ctx->PSSetShader(oPs, nullptr, 0);
    ctx->GSSetShader(oGs, nullptr, 0);
    ctx->VSSetConstantBuffers(13, 1, &oVcb);
    ctx->PSSetConstantBuffers(13, 1, &oPcb);
    ctx->PSSetShaderResources(0, 1, &oSrv);
    ctx->PSSetSamplers(0, 1, &oSmp);
    ctx->RSSetState(oRs);
    ctx->RSSetViewports(1, &oVp);
    ctx->OMSetDepthStencilState(oDs, oRef);
    ctx->OMSetBlendState(oBs, oBf, oMask);
    ctx->IASetInputLayout(oIl);
    ctx->IASetPrimitiveTopology(oTopo);
    IUnknown *rel[] = {oRtv, oDsv, oVs, oPs, oVcb, oPcb, oSrv, oSmp, oRs, oDs, oBs, oIl, oGs};
    for (auto *x : rel)
        if (x) x->Release();
}

void Install() {
    if (!BuildMatches()) return;
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void *>(FN_WeaponDraw), (void *)hk_WeaponDraw, (void **)&o_WeaponDraw);
    MH_EnableHook(reinterpret_cast<void *>(FN_WeaponDraw));
    PVR_LOG("hand: WeaponDraw hook %d", (int)s);
}

}  // namespace pvr::hand
