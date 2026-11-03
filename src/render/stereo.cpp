// Stereo world rendering: the gameplay path builds one render view then calls
// DrawWorld(view, sector). We replace that call with two, one per eye, each with the
// view matrix rebuilt from (game position + game yaw, horizon-level) * HMD eye pose, and
// the projection replaced by the eye's asymmetric FOV. Color/depth targets are
// snapshotted before the first eye and restored before the second.
#include "render/stereo.h"
#include "render/hand_weapon.h"
#include "render/overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstring>
#include <mutex>

#include "common/config.h"
#include "common/log.h"
#include "common/vrmath.h"
#include "game/game_addr.h"
#include "game/hud_state.h"
#include "hooks/d3d_hooks.h"
#include "input/pad_state.h"
#include "xr/xr_session.h"

namespace pvr::stereo {
using namespace pvr::game;

using PFN_DrawWorld = void(__fastcall *)(uint8_t *view, void *sector);
using PFN_ViewFn = void(__fastcall *)(uint8_t *view);
using PFN_ViewToConsts = void(__fastcall *)(uint8_t *view, void *consts);
using PFN_Upload = void(__fastcall *)(void *renderer, void *cb, void *data, uint64_t, uint64_t);

static PFN_DrawWorld o_DrawWorld = nullptr;
static auto ViewFinalize = reinterpret_cast<PFN_ViewFn>(FN_ViewFinalize);
static auto ViewToConsts = reinterpret_cast<PFN_ViewToConsts>(FN_ViewToConstants);

using vm::M4;
using vm::Mul;
using vm::InvRigid;
using vm::Norm3;
using vm::PoseToM4;

// ---- derived per-view data (rotation-only view, sky matrices, camera quaternion) ----
using M3 = float[3][3];
static void Get3(const float *m4, M3 o) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o[r][c] = m4[r * 4 + c];
}
static void Set3(float *m4, const M3 a) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m4[r * 4 + c] = a[r][c];
}
static void Mul3(const M3 a, const M3 b, M3 o) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}
static bool Inv3(const M3 m, M3 o) {
    float d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
              m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(d) < 1e-8f) return false;
    float id = 1.0f / d;
    o[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    o[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    o[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    o[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    o[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    o[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    o[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    o[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    o[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return true;
}
static void QuatToM3(const float *q, M3 R) {  // q = x,y,z,w ; column-vector convention
    float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z), R[0][1] = 2 * (x * y - z * w), R[0][2] = 2 * (x * z + y * w);
    R[1][0] = 2 * (x * y + z * w), R[1][1] = 1 - 2 * (x * x + z * z), R[1][2] = 2 * (y * z - x * w);
    R[2][0] = 2 * (x * z - y * w), R[2][1] = 2 * (y * z + x * w), R[2][2] = 1 - 2 * (x * x + y * y);
}
static void M3ToQuat(const M3 R, float *q) {  // inverse of QuatToM3
    float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        q[3] = 0.25f * s, q[0] = (R[2][1] - R[1][2]) / s, q[1] = (R[0][2] - R[2][0]) / s, q[2] = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        float s = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2;
        q[3] = (R[2][1] - R[1][2]) / s, q[0] = 0.25f * s, q[1] = (R[0][1] + R[1][0]) / s, q[2] = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        float s = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2;
        q[3] = (R[0][2] - R[2][0]) / s, q[0] = (R[0][1] + R[1][0]) / s, q[1] = 0.25f * s, q[2] = (R[1][2] + R[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2;
        q[3] = (R[1][0] - R[0][1]) / s, q[0] = (R[0][2] + R[2][0]) / s, q[1] = (R[1][2] + R[2][1]) / s, q[2] = 0.25f * s;
    }
}
static float Diff3(const M3 a, const M3 b) {
    float d = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) d += std::fabs(a[r][c] - b[r][c]);
    return d;
}

// The quaternion at +0x3e0 relates to the view rotation through one of a few conventions;
// detect it once from the game's own data. 0 = unknown, 1 = R(q) = camera->world rows (C),
// 2 = R(q) = V rotation (world->camera), 3 = negated-q variants are equivalent so no case.
static int g_quatConv = 0;
static int g_billboardMode = 1;
static int g_quatProbe = 0;

static void UpdateDerived(const uint8_t *gameView, uint8_t *eyeView, const M4 &Veye, float k) {
    const float *Vg = reinterpret_cast<const float *>(gameView + VW_ViewMatrix);
    M3 Rg, Re, RgInv, D;
    Get3(Vg, Rg);
    Get3(Veye.m, Re);
    // 1) rotation-only view copy at +0x160
    float *rot = reinterpret_cast<float *>(eyeView + VW_ViewRot);
    Set3(rot, Re);
    // 2) sprite billboard matrices (+0x1e0, +0x220; CPU-side in the sprite pass): upright,
    // built from game yaw only -> rotate them about world Z by (eye yaw - game yaw).
    {
        // forward = -(camera back row); camera->world rows are the columns of V
        float fg[2] = {-Rg[0][2], -Rg[1][2]};
        float fe[2] = {-Re[0][2], -Re[1][2]};
        float a = std::atan2(fe[1], fe[0]) - std::atan2(fg[1], fg[0]);
        float c = std::cos(a), s = std::sin(a);
        M3 Rz = {{c, s, 0}, {-s, c, 0}, {0, 0, 1}};  // row-vector rotation by +a about Z
        M3 RzT = {{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
        static int s_bbLog = 0;
        if (s_bbLog++ < 2) {
            const float *S0 = reinterpret_cast<const float *>(gameView + VW_Sky0);
            const float *S1 = reinterpret_cast<const float *>(gameView + VW_Sky1);
            PVR_LOG("billboard: game fwd=(%.2f %.2f) right col0 V=(%.2f %.2f %.2f) | S0 rows (%.2f %.2f %.2f)(%.2f %.2f %.2f)(%.2f %.2f %.2f) | S1 rows (%.2f %.2f %.2f)(%.2f %.2f %.2f)(%.2f %.2f %.2f)",
                    fg[0], fg[1], Rg[0][0], Rg[1][0], Rg[2][0], S0[0], S0[1], S0[2], S0[4], S0[5], S0[6], S0[8], S0[9],
                    S0[10], S1[0], S1[1], S1[2], S1[4], S1[5], S1[6], S1[8], S1[9], S1[10]);
        }
        // mode 0: untouched, 1: S*Rz, 2: S*Rz^T, 3: Rz*S, 4: Rz^T*S (cycled live with L-grip + L3)
        int mode = g_billboardMode;
        for (int off : {VW_Sky0, VW_Sky1}) {
            if (mode == 0) break;
            M3 S, O;
            Get3(reinterpret_cast<const float *>(gameView + off), S);
            if (mode == 1) Mul3(S, Rz, O);
            else if (mode == 2) Mul3(S, RzT, O);
            else if (mode == 3) Mul3(Rz, S, O);
            else Mul3(RzT, S, O);
            Set3(reinterpret_cast<float *>(eyeView + off), O);
        }
    }
    (void)RgInv;
    (void)D;
    // 3) camera quaternion: proper view rotation Rp = X(q) * B for a fixed axis-swap B, where
    // X is R(q) or R(q)^T. Solve B from the game's frame (needs a non-symmetric pose), keep the
    // X choice whose B is a clean signed permutation, then q_eye from X(q_eye) = Rep * B^-1.
    M3 Rgp, Rep;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            Rgp[r][c] = Rg[r][c] * (c == 2 ? 1.0f / k : 1.0f), Rep[r][c] = Re[r][c] * (c == 2 ? 1.0f / k : 1.0f);
    static M3 s_Binv;
    if (g_quatConv == 0 && g_quatProbe < 2000) {
        ++g_quatProbe;
        const float *qg = reinterpret_cast<const float *>(gameView + VW_Quat);
        M3 X[2], Xi, B[2];
        QuatToM3(qg, X[0]);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) X[1][r][c] = X[0][c][r];
        bool symmetric = Diff3(X[0], X[1]) < 0.05f;  // pure 0/180-degree pose: can't tell X apart
        int good = 0, which = 0;
        for (int i = 0; i < 2; ++i) {
            if (!Inv3(X[i], Xi)) continue;
            Mul3(Xi, Rgp, B[i]);
            bool perm = true;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    float a = std::fabs(B[i][r][c]);
                    if (a > 0.03f && std::fabs(a - 1.0f) > 0.03f) perm = false;
                }
            if (perm) ++good, which = i + 1;
        }
        if (g_quatProbe <= 3 || (g_quatProbe % 200) == 0)
            PVR_LOG("quat probe %d: q=(%.3f %.3f %.3f %.3f) symmetric=%d permutations=%d", g_quatProbe, qg[0], qg[1],
                    qg[2], qg[3], (int)symmetric, good);
        if (!symmetric && good == 1) {
            g_quatConv = which;
            Inv3(B[which - 1], s_Binv);
            PVR_LOG("quat convention: X=%s, B rows (%.0f %.0f %.0f)(%.0f %.0f %.0f)(%.0f %.0f %.0f)",
                    which == 1 ? "R(q)" : "R(q)^T", B[which - 1][0][0], B[which - 1][0][1], B[which - 1][0][2],
                    B[which - 1][1][0], B[which - 1][1][1], B[which - 1][1][2], B[which - 1][2][0], B[which - 1][2][1],
                    B[which - 1][2][2]);
        }
    }
    if (g_quatConv) {
        M3 Xe, R;
        Mul3(Rep, s_Binv, Xe);
        if (g_quatConv == 1) memcpy(R, Xe, sizeof(R));
        else
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) R[r][c] = Xe[c][r];
        float *q = reinterpret_cast<float *>(eyeView + VW_Quat);
        const float *qg = reinterpret_cast<const float *>(gameView + VW_Quat);
        M3ToQuat(R, q);
        // keep the same hemisphere as the game's quaternion
        if (q[0] * qg[0] + q[1] * qg[1] + q[2] * qg[2] + q[3] * qg[3] < 0)
            for (int i = 0; i < 4; ++i) q[i] = -q[i];
    }
}

// ---- Sky dome (progs/SlaveSkyDome.shader) ----
// The game's shader builds a symmetric-frustum ray and rotates it by lookRot(-yaw, pitch) from
// the player actor (+0x9c, +0x94). That can't express head roll or the asymmetric eye frustum,
// so during the sky draw we swap in a pixel shader that builds the exact per-eye ray and maps it
// into the same dome space: dome = L(actor yaw, pitch) * Cgame * Ceye^T * eyeRay. The game's
// yaw cancels in L*Cgame, so the sky is fixed to the world. Texture/UV math is unchanged.
// HUD (status bar, messages) is drawn inside DrawWorld once per eye, baked into the eye image at
// the screen edges. In stereo: eye 0 draws it into the overlay layer, eye 1 skips it.
static ID3D11Texture2D *TexOf(ID3D11View *v);
using PFN_HudDraw = void(__fastcall *)(void *, void *, void *, void *);
static PFN_HudDraw o_HudDraw = nullptr;
static int g_hudEye = -1;
// While the HUD is drawn into its canvas, widen its 2D ortho (about the centre) by g_orthoWiden so
// the canvas covers a wider screen (HUD laid out for widescreen; the quad gets that aspect back).
using PFN_Ortho = void(__fastcall *)(float *, float, float, float, float, float, float);
static PFN_Ortho o_Ortho = nullptr;
static float g_orthoWiden = 0;
static void __fastcall hk_Ortho(float *m, float l, float r, float b, float t, float n, float f) {
    o_Ortho(m, l, r, b, t, n, f);
    if (g_hudEye == 0) {
        static float s_last[4] = {};
        static int s_logs = 0;
        if (s_logs < 12 && (s_last[0] != l || s_last[1] != r || s_last[2] != b || s_last[3] != t)) {
            ++s_logs;
            s_last[0] = l, s_last[1] = r, s_last[2] = b, s_last[3] = t;
            PVR_LOG("hud ortho: l %.1f r %.1f b %.1f t %.1f, widen x%.3f", l, r, b, t, g_orthoWiden);
        }
    }
    if (g_orthoWiden <= 0) return;
    m[0] /= g_orthoWiden;
    m[12] /= g_orthoWiden;
    if (Cfg().hudOrthoY > 0) m[5] /= Cfg().hudOrthoY, m[13] /= Cfg().hudOrthoY;
}
static void __fastcall hk_HudDraw(void *a, void *b, void *c, void *d) {
    if (g_hudEye <= 0) hudstate::Capture(a);  // numbers for the wrist gauges
    if (g_hudEye < 0 || !Cfg().hudOverlay) return o_HudDraw(a, b, c, d);
    if (g_hudEye == 1) return;
    ID3D11DeviceContext *ctx = nullptr;
    GameDevice()->GetImmediateContext(&ctx);
    ID3D11RenderTargetView *prevRtv = nullptr;
    ID3D11DepthStencilView *prevDsv = nullptr;
    ctx->OMGetRenderTargets(1, &prevRtv, &prevDsv);
    ID3D11Texture2D *t = TexOf(prevRtv);
    ID3D11RenderTargetView *ov = t ? overlay::Prepare(ctx, t) : nullptr;
    if (ov && Cfg().hudNoRedirect) {
        o_HudDraw(a, b, c, d);
        ID3D11Resource *dst = nullptr;
        ov->GetResource(&dst);
        if (dst) ctx->CopyResource(dst, t), dst->Release();
    } else if (ov) {
        SetRenderTargetRedirect(ov);
        ctx->OMSetRenderTargets(1, &ov, nullptr);
        static ULONGLONG s_next = 0;
        static int s_logs = 0;
        bool log = s_logs < 3 && GetTickCount64() >= s_next;
        if (log) {
            ++s_logs, s_next = GetTickCount64() + 20000;
            D3D11_VIEWPORT vp{};
            UINT nv = 1;
            ctx->RSGetViewports(&nv, &vp);
            PVR_LOG("hud draw: rt %p vp(%.0f,%.0f %.0fx%.0f)", (void *)t, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
            __try {
                uint8_t *hud = (uint8_t *)a;
                PVR_LOG("hud state: smoothed hp %.3f +0xa4 %d", *(float *)(hud + 0x10), *(int *)(hud + 0xa4));
            } __except (1) {
            }
            LogDraws(40, "hud");
        }
        D3D11_TEXTURE2D_DESC td;
        t->GetDesc(&td);
        float rtAspect = (float)td.Width / (float)td.Height;
        g_orthoWiden = Cfg().hudAspect > rtAspect ? Cfg().hudAspect / rtAspect : 0.0f;
        o_HudDraw(a, b, c, d);
        g_orthoWiden = 0;
        if (log) LogDraws(0, "");
        SetRenderTargetRedirect(nullptr);
        ctx->OMSetRenderTargets(1, &prevRtv, prevDsv);
    } else {
        o_HudDraw(a, b, c, d);
    }
    if (t) t->Release();
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
    ctx->Release();
}

// Automap: drawn per eye in screen space inside DrawWorld. Eye 0 draws it once into the menu layer (the square,
// head-locked menu panel); eye 1 skips it.
static PFN_HudDraw o_AutomapDraw = nullptr;
static void __fastcall hk_AutomapDraw(void *a, void *b, void *c, void *d) {
    if (g_hudEye < 0 || !Cfg().hudOverlay) return o_AutomapDraw(a, b, c, d);
    if (g_hudEye == 1) return;
    ID3D11DeviceContext *ctx = nullptr;
    GameDevice()->GetImmediateContext(&ctx);
    ID3D11RenderTargetView *prevRtv = nullptr;
    ID3D11DepthStencilView *prevDsv = nullptr;
    ctx->OMGetRenderTargets(1, &prevRtv, &prevDsv);
    ID3D11Texture2D *t = TexOf(prevRtv);
    ID3D11RenderTargetView *ov = t ? overlay::PrepareMenu(ctx, t) : nullptr;
    if (ov) {
        SetRenderTargetRedirect(ov);
        ctx->OMSetRenderTargets(1, &ov, nullptr);
        o_AutomapDraw(a, b, c, d);
        SetRenderTargetRedirect(nullptr);
        ctx->OMSetRenderTargets(1, &prevRtv, prevDsv);
    } else {
        o_AutomapDraw(a, b, c, d);
    }
    if (t) t->Release();
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
    ctx->Release();
}

using PFN_SkyPass = void(__fastcall *)(uint8_t *view);
using PFN_Player = void *(__fastcall *)();
using PFN_GetActor = uint8_t *(__fastcall *)(void *player);
static PFN_SkyPass o_SkyPass = nullptr;
constexpr int ACT_Pitch = 0x94, ACT_Yaw = 0x9c;
constexpr int VW_Width = 0x400, VW_Height = 0x404;

struct SkyEye {
    bool active = false;
    float Cg[3][3];  // game camera axes in world (rows r, u, b), orthonormal
    float Ce[3][3];  // eye camera axes in world
    float tl, tr, tu, td;
};
static SkyEye g_sky;

static const char *kSkyPS = R"(
cbuffer PVRSky : register(b13) { float4 vp; float4 tans; float4 A0; float4 A1; float4 A2; };
Texture2D uTex_tDefaultTex : register(t0);
SamplerState uSampletDefaultTex : register(s0);
#define PI 3.14159265359
float myAtan2(float y, float x) {
         if(x>0.0) { return atan(y/x); }
    else if(y>0.0) { return PI/2.0-atan(x/y); }
    else if(y<0.0) { return -PI/2.0 -atan(x/y); }
    else if(x<0.0) { return atan(y/x)+PI; }
    return 0.0;
}
float2 dirToDomeUv(float3 d) {
    float2 uv = float2(myAtan2(d.z, d.x) / (2.0 * PI) + 0.5, acos(-d.y) / PI);
    uv.y = uv.y * 2.0 - 1.0;
    return uv;
}
float4 main(float4 pos : SV_POSITION, float fov : FOV0, float2 tc : TEXCOORD0, float3x3 rot : ROT0) : SV_TARGET {
    float u = pos.x * vp.x, v = pos.y * vp.y;
    float3 ray = float3(lerp(tans.x, tans.y, u), lerp(tans.z, tans.w, v), -1.0);
    float3 rd = normalize(float3(dot(A0.xyz, ray), dot(A1.xyz, ray), dot(A2.xyz, ray)));
    const float2 offset = float2(0.125, 0.725);
    float2 skyUv = (dirToDomeUv(rd) + offset) * float2(4.0, 0.7);
    skyUv.y = clamp(skyUv.y, 0.002, 0.998);  // extend the band instead of showing the empty colour
    return float4(uTex_tDefaultTex.SampleLevel(uSampletDefaultTex, skyUv, 0).rgb, 1.0);
}
)";
static ID3D11PixelShader *g_skyPS = nullptr;
static ID3D11Buffer *g_skyCB = nullptr;
static bool g_skyInitFailed = false;

static bool SkyInit() {
    if (g_skyPS) return true;
    if (g_skyInitFailed) return false;
    ID3DBlob *b = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kSkyPS, strlen(kSkyPS), "pvr_sky", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &err))) {
        PVR_LOG("sky: shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        g_skyInitFailed = true;
        return false;
    }
    GameDevice()->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_skyPS);
    b->Release();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    GameDevice()->CreateBuffer(&bd, nullptr, &g_skyCB);
    PVR_LOG("sky: VR sky shader ready");
    return g_skyPS && g_skyCB;
}

static void __fastcall hk_SkyPass(uint8_t *view) {
    if (!g_sky.active || !SkyInit()) return o_SkyPass(view);
    // dome space: +Y up (acos(-d.y)), azimuth in XZ. World is Z-up. Fixed, proper mapping:
    // dome = (world.x, world.z, -world.y)  =>  A = W * Ce^T (eye ray -> world -> dome)
    M3 W = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    M3 CeT, A;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) CeT[r][c] = g_sky.Ce[c][r];
    Mul3(W, CeT, A);
    float w = *reinterpret_cast<float *>(view + VW_Width), h = *reinterpret_cast<float *>(view + VW_Height);
    float data[20] = {w > 0 ? 1.0f / w : 0, h > 0 ? 1.0f / h : 0, 0, 0,
                      g_sky.tl, g_sky.tr, g_sky.tu, g_sky.td,
                      A[0][0], A[0][1], A[0][2], 0, A[1][0], A[1][1], A[1][2], 0, A[2][0], A[2][1], A[2][2], 0};
    ID3D11DeviceContext *ctx = nullptr;
    GameDevice()->GetImmediateContext(&ctx);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_skyCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, sizeof(data));
        ctx->Unmap(g_skyCB, 0);
    }
    ctx->Release();
    static int s_log = 0;
    if (s_log++ < 2) PVR_LOG("sky: view %gx%g tans %.3f %.3f %.3f %.3f", w, h, g_sky.tl, g_sky.tr, g_sky.tu, g_sky.td);
    SetPixelShaderOverride(g_skyPS, g_skyCB, 13);
    o_SkyPass(view);
    SetPixelShaderOverride(nullptr, nullptr, 0);
}

// ---- D3D11 target snapshot/restore ----
struct Snap {
    ID3D11Texture2D *tex = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
};
static Snap g_colorSnap, g_depthSnap;

static ID3D11Texture2D *TexOf(ID3D11View *v) {
    if (!v) return nullptr;
    ID3D11Resource *r = nullptr;
    v->GetResource(&r);
    ID3D11Texture2D *t = nullptr;
    if (r) {
        r->QueryInterface(IID_PPV_ARGS(&t));
        r->Release();
    }
    return t;
}
static bool EnsureSnap(Snap &s, ID3D11Texture2D *src) {
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    if (s.tex && s.desc.Width == d.Width && s.desc.Height == d.Height && s.desc.Format == d.Format &&
        s.desc.SampleDesc.Count == d.SampleDesc.Count)
        return true;
    if (s.tex) s.tex->Release(), s.tex = nullptr;
    D3D11_TEXTURE2D_DESC c = d;
    c.BindFlags = 0;
    c.CPUAccessFlags = 0;
    c.MiscFlags = 0;
    c.Usage = D3D11_USAGE_DEFAULT;
    // depth formats need a bind flag to be creatable in some cases; keep typeless-compatible desc
    if (FAILED(GameDevice()->CreateTexture2D(&c, nullptr, &s.tex))) {
        c.BindFlags = d.BindFlags;
        if (FAILED(GameDevice()->CreateTexture2D(&c, nullptr, &s.tex))) return false;
    }
    s.desc = d;
    return true;
}

static int g_logBudget = 20;
static std::mutex g_baseMtx;
static float g_base[4][3];  // rows: right, up, back (LOCAL axes in world), origin
static bool g_baseValid = false;

static FrameStats g_stats;
bool LastStats(FrameStats &out) {
    std::lock_guard<std::mutex> lk(g_baseMtx);
    if (!g_stats.count) return false;
    out = g_stats;
    return true;
}

bool BaseFrame(float rows[4][3]) {
    std::lock_guard<std::mutex> lk(g_baseMtx);
    if (!g_baseValid) return false;
    memcpy(rows, g_base, sizeof(g_base));
    return true;
}

static void __fastcall hk_DrawWorld(uint8_t *view, void *sector) {
    {
        static bool s_prev = false;
        PadState ps = PadSnapshot();
        bool combo = ps.buttons[PB_PADDLE2] && ps.buttons[PB_LSTICK];
        if (combo && !s_prev) {
            g_billboardMode = (g_billboardMode + 1) % 5;
            PVR_LOG("billboard mode -> %d", g_billboardMode);
        }
        s_prev = combo;
    }
    XrView views[2];
    if (!Cfg().stereo || !xr::FrameViews(views)) {
        o_DrawWorld(view, sector);
        return;
    }
    ID3D11DeviceContext *ctx = nullptr;
    GameDevice()->GetImmediateContext(&ctx);

    // ---- base camera frame: game origin + horizon-level game yaw ----
    const float *V = reinterpret_cast<const float *>(view + VW_ViewMatrix);
    const float *org = reinterpret_cast<const float *>(view + VW_Origin);
    // camera->world rows are the columns of V (V = inv(C), orthogonal 3x3)
    float r[3] = {V[0], V[4], V[8]};
    float u[3] = {V[1], V[5], V[9]};
    float b[3] = {V[2], V[6], V[10]};
    float det = r[0] * (u[1] * b[2] - u[2] * b[1]) - r[1] * (u[0] * b[2] - u[2] * b[0]) + r[2] * (u[0] * b[1] - u[1] * b[0]);
    // the engine scales (and possibly flips) camera z; |b| = scale, sign(det) = flip
    float k = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]) * (det < 0 ? -1.0f : 1.0f);
    float rh[3] = {r[0], r[1], 0};
    Norm3(rh);
    float uw[3] = {0, 0, 1};
    float bh[3] = {rh[1] * uw[2] - rh[2] * uw[1], rh[2] * uw[0] - rh[0] * uw[2], rh[0] * uw[1] - rh[1] * uw[0]};
    M4 Cbase{};
    for (int i = 0; i < 3; ++i) {
        Cbase.at(0, i) = rh[i];
        Cbase.at(1, i) = uw[i];
        Cbase.at(2, i) = bh[i];
        Cbase.at(3, i) = org[i];
    }
    Cbase.at(3, 3) = 1;
    {
        std::lock_guard<std::mutex> lk(g_baseMtx);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) g_base[r][c] = Cbase.at(r, c);
        g_baseValid = true;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) g_stats.base[r][c] = Cbase.at(r, c);
        g_stats.k = k;
        float kk = std::fabs(k) > 1e-6f ? k : 1.0f;
        for (int i = 0; i < 3; ++i) g_stats.gameFwd[i] = -b[i] / kk;
        ++g_stats.count;
    }

    if (g_logBudget > 0) {
        --g_logBudget;
        PVR_LOG("stereo: det=%.3f r=(%.2f %.2f %.2f) u=(%.2f %.2f %.2f) b=(%.2f %.2f %.2f) org=(%.1f %.1f %.1f)", det,
                r[0], r[1], r[2], u[0], u[1], u[2], b[0], b[1], b[2], org[0], org[1], org[2]);
    }

    // ---- snapshot targets as they are now (the state each eye must start from) ----
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Texture2D *colorTex = TexOf(rtv), *depthTex = TexOf(dsv);
    bool haveColor = colorTex && EnsureSnap(g_colorSnap, colorTex);
    bool haveDepth = depthTex && EnsureSnap(g_depthSnap, depthTex);
    if (haveColor) ctx->CopyResource(g_colorSnap.tex, colorTex);
    if (haveDepth) ctx->CopyResource(g_depthSnap.tex, depthTex);

    alignas(16) uint8_t eyeView[VW_Size + 0x20];
    alignas(16) uint8_t consts[ViewConstantsSize + 0x10];
    void *renderer = *reinterpret_cast<void **>(G_Renderer);
    void *cb = *reinterpret_cast<void **>(G_ViewConstBuffer);
    auto upload = (*reinterpret_cast<PFN_Upload **>(renderer))[RendererUploadVtbl / 8];
    const float scale = Cfg().worldScale;

    ID3D11Texture2D *lastScene = nullptr;
    ID3D11Texture2D *hudRT = nullptr;
    for (int eye = 0; eye < 2; ++eye) {
        if (eye == 1) {
            if (haveColor) ctx->CopyResource(colorTex, g_colorSnap.tex);
            if (haveDepth) ctx->CopyResource(depthTex, g_depthSnap.tex);
        }
        memcpy(eyeView, view, VW_Size);
        M4 E = PoseToM4(views[eye].pose, scale);
        M4 Ceye = Mul(E, Cbase);  // eye -> world (proper rotation)
        M4 Veye = InvRigid(Ceye);
        for (int rr = 0; rr < 4; ++rr) Veye.at(rr, 2) *= k;  // re-apply engine camera-z scale/flip
        memcpy(eyeView + VW_ViewMatrix, Veye.m, sizeof(Veye.m));
        float *o = reinterpret_cast<float *>(eyeView + VW_Origin);
        o[0] = Ceye.at(3, 0), o[1] = Ceye.at(3, 1), o[2] = Ceye.at(3, 2);
        {
            std::lock_guard<std::mutex> lk(g_baseMtx);
            for (int i = 0; i < 3; ++i) {
                g_stats.eyeOrigin[eye][i] = Ceye.at(3, i);
                g_stats.eyeFwd[eye][i] = -Ceye.at(2, i);
                g_stats.eyeUp[eye][i] = Ceye.at(1, i);
            }
        }
        UpdateDerived(view, eyeView, Veye, k);

        // sky parameters for this eye (consumed by hk_SkyPass inside the world draw)
        {
            const XrFovf &ff = views[eye].fov;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    g_sky.Ce[r][c] = Ceye.at(r, c);
                    g_sky.Cg[r][c] = V[c * 4 + r] / (r == 2 ? k : 1.0f);  // columns of V = camera axes
                }
            g_sky.tl = std::tan(ff.angleLeft), g_sky.tr = std::tan(ff.angleRight);
            g_sky.tu = std::tan(ff.angleUp), g_sky.td = std::tan(ff.angleDown);
            g_sky.active = true;
        }

        // asymmetric projection (same conventions as FN_ViewBuildProjection)
        float *P = reinterpret_cast<float *>(eyeView + VW_Projection);
        const XrFovf &f = views[eye].fov;
        float tl = std::tan(f.angleLeft), tr = std::tan(f.angleRight);
        float tu = std::tan(f.angleUp), td = std::tan(f.angleDown);
        P[0] = 2.0f / (tr - tl);
        P[5] = 2.0f / (tu - td);
        P[8] = (tr + tl) / (tr - tl);
        P[9] = (tu + td) / (tu - td);

        ViewFinalize(eyeView);
        ViewToConsts(eyeView, consts);
        upload(renderer, cb, consts, 0, 0);
        int d0 = g_drawCount.load();
        // the RT bound now is where draws start; OMSetRenderTargets inside the call updates it
        {
            ID3D11RenderTargetView *cur = nullptr;
            ctx->OMGetRenderTargets(1, &cur, nullptr);
            ID3D11Resource *res = nullptr;
            if (cur) cur->GetResource(&res);
            TraceBegin(0, res);
            if (res) res->Release();
            if (cur) cur->Release();
        }
        hand::SetEye(eye);
        g_hudEye = eye;
        o_DrawWorld(eyeView, sector);
        g_hudEye = -1;
        hand::SetEye(-1);
        ID3D11Resource *sceneRT = TraceEnd(0, g_logBudget > 0 && eye == 0);
        // the HUD is drawn after the world into the engine's final composite target, which is
        // the last target DrawWorld itself drew to (post-process output), not the scene target
        if (ID3D11Resource *lastRT = TraceLast(0)) {
            if (hudRT) hudRT->Release();
            hudRT = nullptr;
            lastRT->QueryInterface(IID_PPV_ARGS(&hudRT));
        }
        if (g_logBudget > 0 && eye == 0) PVR_LOG("stereo: hud target %p (scene %p)", (void *)hudRT, (void *)sceneRT);
        if (g_logBudget > 0) PVR_LOG("stereo: eye %d world draw issued %d draws, scene rt %p", eye, g_drawCount.load() - d0, (void *)sceneRT);

        // capture the eye from the target that received most of the world's draws
        ID3D11RenderTargetView *outRtv = nullptr;
        ctx->OMGetRenderTargets(1, &outRtv, nullptr);
        ID3D11Texture2D *outTex = nullptr;
        if (sceneRT) sceneRT->QueryInterface(IID_PPV_ARGS(&outTex));
        if (!outTex) outTex = TexOf(outRtv);
        if (outTex) {
            // weapon card in the hand, drawn into this eye's scene with its view-projection
            float *Pm = reinterpret_cast<float *>(eyeView + VW_Projection);
            M4 Pmat;
            memcpy(Pmat.m, Pm, sizeof(Pmat.m));
            M4 VP = Mul(Veye, Pmat);
            auto ndcZ = [&](float z) {
                float cz = z * Pm[10] + Pm[14], cw = z * Pm[11] + Pm[15];
                return cw != 0 ? cz / cw : 0.0f;
            };
            bool reversed = ndcZ(-10.0f) > ndcZ(-1000.0f);
            float cam[3] = {Ceye.at(3, 0), Ceye.at(3, 1), Ceye.at(3, 2)};
            hand::DrawCard(ctx, VP.m, cam, outTex, dsv, reversed);
        }
        if (g_logBudget > 0 && eye == 0)
            PVR_LOG("stereo: rt in=%p out=%p depth=%p", (void *)colorTex, (void *)outTex, (void *)depthTex);
        if (outTex) {
            xr::CaptureEye(eye, outTex);
            if (lastScene) lastScene->Release();
            lastScene = outTex;  // keep the reference for the overlay
        }
        if (outRtv) outRtv->Release();
    }

    g_sky.active = false;
    // HUD / menus / flashes drawn after this point into the scene target go to the VR overlay
    if (lastScene) {
        if (Cfg().hudOverlay) overlay::Begin(ctx, hudRT ? hudRT : lastScene);
        lastScene->Release();
    }
    if (hudRT) hudRT->Release();
    // restore the game's own view constants for anything drawn after the world
    ViewToConsts(view, consts);
    upload(renderer, cb, consts, 0, 0);

    if (colorTex) colorTex->Release();
    if (depthTex) depthTex->Release();
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    ctx->Release();
}

void Install() {
    if (!BuildMatches()) {
        PVR_LOG("stereo: exe build mismatch, stereo disabled");
        return;
    }
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void *>(FN_DrawWorld), (void *)hk_DrawWorld, (void **)&o_DrawWorld);
    MH_EnableHook(reinterpret_cast<void *>(FN_DrawWorld));
    PVR_LOG("stereo: DrawWorld hook %d", (int)s);
    s = MH_CreateHook(reinterpret_cast<void *>(FN_SkyPass), (void *)hk_SkyPass, (void **)&o_SkyPass);
    MH_EnableHook(reinterpret_cast<void *>(FN_SkyPass));
    PVR_LOG("stereo: SkyPass hook %d", (int)s);
    s = MH_CreateHook(reinterpret_cast<void *>(FN_HudDraw), (void *)hk_HudDraw, (void **)&o_HudDraw);
    MH_EnableHook(reinterpret_cast<void *>(FN_HudDraw));
    PVR_LOG("stereo: HudDraw hook %d", (int)s);
    s = MH_CreateHook(reinterpret_cast<void *>(FN_AutomapDraw), (void *)hk_AutomapDraw, (void **)&o_AutomapDraw);
    MH_EnableHook(reinterpret_cast<void *>(FN_AutomapDraw));
    PVR_LOG("stereo: AutomapDraw hook %d", (int)s);
    s = MH_CreateHook(reinterpret_cast<void *>(FN_Ortho), (void *)hk_Ortho, (void **)&o_Ortho);
    MH_EnableHook(reinterpret_cast<void *>(FN_Ortho));
    PVR_LOG("stereo: Ortho hook %d", (int)s);
}

}  // namespace pvr::stereo

namespace pvr::game {
bool BuildMatches() {
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    bool ok = nt->FileHeader.TimeDateStamp == kExpectedTimestamp && (uintptr_t)base == 0x140000000;
    if (!ok) PVR_LOG("build check: timestamp 0x%08X base %p", nt->FileHeader.TimeDateStamp, base);
    return ok;
}
}  // namespace pvr::game
