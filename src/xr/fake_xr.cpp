// FakeXR harness: scripted head/hands/pad + numeric invariant checks (see fake_xr.h).
//
// Script (game dir, [Debug] Script=): one command per line, "<t> <command> [args]", '#' comments.
// Commands run in order; <t> is seconds since the script clock was last reset (start, or `world`).
//   world                      wait until the stereo world draw is running, then reset the clock
//   head <yaw> <pitch> [dur]   head orientation in degrees (yaw + = left, pitch + = up), lerped over dur
//   headpos <x> <y> <z> [dur]  head position in meters (LOCAL)
//   hand <yaw> <pitch> [dur]   right controller aim orientation (LOCAL, absolute)
//   press <button> [dur]       A B X Y BACK START LSTICK RSTICK LB RB UP DOWN LEFT RIGHT PADDLE1 PADDLE2
//   axis <axis> <v> <dur>      LX LY RX RY LT RT, v in -1..1 (SDL convention: LY + = down)
//   check [label]              log invariant checks for the next rendered frame
//   key <name>                 post a key press to the game window: ENTER DOWN UP LEFT RIGHT ESC F10 F11
//   log <text>                 copy text to the log
//   quit                       terminate the game (no config write)
#include "xr/fake_xr.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "common/vrmath.h"
#include "game/aim.h"
#include "hooks/d3d_hooks.h"
#include "input/pad_state.h"
#include "render/stereo.h"
#include "xr/xr_input.h"

namespace pvr::fake {

constexpr float kDeg = 3.14159265f / 180.0f;
constexpr float kIpd = 0.064f;

struct Cmd {
    float t = 0;
    std::string op;
    std::vector<std::string> a;
    int line = 0;
};

struct Lerp {  // one scripted value moving from `from` to `to` over [t0, t0+dur]
    float from[3] = {}, to[3] = {};
    double t0 = 0, dur = 0;
    void Set(const float *now, const float *target, double t, double d) {
        for (int i = 0; i < 3; ++i) from[i] = now[i], to[i] = target[i];
        t0 = t, dur = d;
    }
    void Eval(double t, float *out) const {
        float u = dur > 0 ? (float)std::fmin(1.0, std::fmax(0.0, (t - t0) / dur)) : 1.0f;
        for (int i = 0; i < 3; ++i) out[i] = from[i] + (to[i] - from[i]) * u;
    }
    bool Moving(double t) const { return dur > 0 && t < t0 + dur; }
};

static bool g_init = false;
static std::vector<Cmd> g_cmds;
static size_t g_next = 0;
static double g_clock0 = 0;
static bool g_waitWorld = false;
static Lerp g_head, g_headPos, g_hand;  // head/hand: yaw,pitch,(unused) in degrees
static double g_btnUntil[PB_COUNT] = {};
static double g_axisUntil[PA_COUNT] = {};
static float g_axisVal[PA_COUNT] = {};
static std::string g_checkLabel;
static bool g_checkPending = false;
static double g_lastAuto = 0;
static int g_checkNo = 0;
static unsigned long long g_aimCountAtCheck = 0;
// what the frame being checked was rendered with
static float g_rHead[3] = {}, g_rHeadPos[3] = {}, g_rHand[3] = {};
static bool g_rMoving = true;

static double Now() {
    static LARGE_INTEGER f{}, t0{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f), QueryPerformanceCounter(&t0);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - t0.QuadPart) / double(f.QuadPart);
}

static int ButtonId(const std::string &s) {
    static const char *names[] = {"A", "B", "X", "Y", "BACK", "GUIDE", "START", "LSTICK", "RSTICK", "LB", "RB",
                                  "UP", "DOWN", "LEFT", "RIGHT", "MISC1", "PADDLE1", "PADDLE2", "PADDLE3", "PADDLE4",
                                  "TOUCHPAD"};
    for (int i = 0; i < PB_COUNT; ++i)
        if (_stricmp(s.c_str(), names[i]) == 0) return i;
    return -1;
}
static int AxisId(const std::string &s) {
    static const char *names[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
    for (int i = 0; i < PA_COUNT; ++i)
        if (_stricmp(s.c_str(), names[i]) == 0) return i;
    return -1;
}

static void Load() {
    g_init = true;
    std::wstring path = Cfg().fakeScript;
    if (path.find(L':') == std::wstring::npos) path = GameDir() + path;
    std::ifstream f(path);
    if (!f) {
        PVR_LOG("fake: no script at %ls (head stays neutral)", path.c_str());
        return;
    }
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        ++n;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        Cmd c;
        c.line = n;
        if (!(ss >> c.t >> c.op)) continue;
        std::string tok;
        if (c.op == "log" || c.op == "check") {
            std::getline(ss, tok);
            size_t b = tok.find_first_not_of(" \t");
            if (b != std::string::npos) c.a.push_back(tok.substr(b));
        } else {
            while (ss >> tok) c.a.push_back(tok);
        }
        g_cmds.push_back(c);
    }
    PVR_LOG("fake: script %ls, %zu commands", path.c_str(), g_cmds.size());
}

static HWND GameWindow() {
    struct S { DWORD pid; HWND h; } s{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto *s = reinterpret_cast<S *>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != s->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            s->h = h;
            return FALSE;
        },
        (LPARAM)&s);
    return s.h;
}
static bool PostKey(const std::string &name) {
    static const struct { const char *n; UINT vk, sc; } keys[] = {
        {"ENTER", 0x0D, 0x1C}, {"DOWN", 0x28, 0x50}, {"UP", 0x26, 0x48}, {"LEFT", 0x25, 0x4B},
        {"RIGHT", 0x27, 0x4D}, {"ESC", 0x1B, 0x01}, {"F10", 0x79, 0x44}, {"F11", 0x7A, 0x57}};
    for (auto &k : keys) {
        if (name != k.n) continue;
        HWND h = GameWindow();
        if (!h) return false;
        PostMessageW(h, WM_KEYDOWN, k.vk, 1 | (k.sc << 16));
        PostMessageW(h, WM_KEYUP, k.vk, 1 | (k.sc << 16) | (3u << 30));
        return true;
    }
    return false;
}

static float Arg(const Cmd &c, size_t i, float def) { return i < c.a.size() ? (float)atof(c.a[i].c_str()) : def; }

static void Exec(const Cmd &c, double now) {
    float cur[3], tgt[3];
    if (c.op == "world") {
        g_waitWorld = true;
    } else if (c.op == "head" || c.op == "hand") {
        Lerp &l = c.op == "head" ? g_head : g_hand;
        l.Eval(now, cur);
        tgt[0] = Arg(c, 0, 0), tgt[1] = Arg(c, 1, 0), tgt[2] = 0;
        l.Set(cur, tgt, now, Arg(c, 2, 0));
    } else if (c.op == "headpos") {
        g_headPos.Eval(now, cur);
        tgt[0] = Arg(c, 0, 0), tgt[1] = Arg(c, 1, 0), tgt[2] = Arg(c, 2, 0);
        g_headPos.Set(cur, tgt, now, Arg(c, 3, 0));
    } else if (c.op == "press") {
        int b = c.a.empty() ? -1 : ButtonId(c.a[0]);
        if (b < 0) return (void)PVR_LOG("fake: line %d: bad button", c.line);
        g_btnUntil[b] = now + Arg(c, 1, 0.15f);
    } else if (c.op == "axis") {
        int a = c.a.empty() ? -1 : AxisId(c.a[0]);
        if (a < 0) return (void)PVR_LOG("fake: line %d: bad axis", c.line);
        g_axisVal[a] = Arg(c, 1, 0);
        g_axisUntil[a] = now + Arg(c, 2, 0.2f);
    } else if (c.op == "check") {
        g_checkPending = true;
        g_checkLabel = c.a.empty() ? "" : c.a[0];
    } else if (c.op == "key") {
        if (c.a.empty() || !PostKey(c.a[0])) return (void)PVR_LOG("fake: line %d: key failed", c.line);
    } else if (c.op == "log") {
        PVR_LOG("fake: %s", c.a.empty() ? "" : c.a[0].c_str());
    } else if (c.op == "quit") {
        PVR_LOG("fake: quit (line %d)", c.line);
        TerminateProcess(GetCurrentProcess(), 0);
    } else {
        PVR_LOG("fake: line %d: unknown command '%s'", c.line, c.op.c_str());
        return;
    }
    PVR_LOG("fake: t=%.2f line %d: %s", now - g_clock0, c.line, c.op.c_str());
}

// ---------------- pixel statistics ----------------
struct Pix {
    bool ok = false;
    UINT w = 0, h = 0;
    std::vector<uint32_t> px;  // RGBA8 (BGRA swapped to RGBA)
};
static Pix Read(ID3D11Texture2D *t) {
    Pix p;
    if (!t) return p;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if ((!bgra && !rgba) || d.SampleDesc.Count != 1) return p;
    D3D11_TEXTURE2D_DESC s = d;
    s.MipLevels = s.ArraySize = 1;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = 0;
    ID3D11Device *dev = GameDevice();
    ID3D11Texture2D *st = nullptr;
    if (FAILED(dev->CreateTexture2D(&s, nullptr, &st))) return p;
    ID3D11DeviceContext *ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        p.w = d.Width, p.h = d.Height;
        p.px.resize((size_t)p.w * p.h);
        for (UINT y = 0; y < p.h; ++y) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            for (UINT x = 0; x < p.w; ++x) {
                uint32_t v = row[x];
                if (bgra) v = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
                p.px[(size_t)y * p.w + x] = v;
            }
        }
        ctx->Unmap(st, 0);
        p.ok = true;
    }
    st->Release();
    ctx->Release();
    return p;
}
static float Luma(uint32_t v) {
    return (0.299f * (v & 0xff) + 0.587f * ((v >> 8) & 0xff) + 0.114f * ((v >> 16) & 0xff)) / 255.0f;
}

// ---------------- invariant checks ----------------
static float AngDeg(const float *a, const float *b) {
    float d = vm::Dot3(a, b) / (vm::Len3(a) * vm::Len3(b) + 1e-12f);
    return std::acos(std::fmax(-1.0f, std::fmin(1.0f, d))) / kDeg;
}
static float WrapDeg(float d) {
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}
static const char *PF(bool ok) { return ok ? "PASS" : "FAIL"; }

static void RunChecks(ID3D11ShaderResourceView *hudSrv, ID3D11Texture2D *eye0, ID3D11Texture2D *eye1) {
    ++g_checkNo;
    const char *label = g_checkLabel.c_str();
    PVR_LOG("INV #%d [%s] head yaw %.1f pitch %.1f pos (%.2f %.2f %.2f) hand yaw %.1f pitch %.1f%s", g_checkNo, label,
            g_rHead[0], g_rHead[1], g_rHeadPos[0], g_rHeadPos[1], g_rHeadPos[2], g_rHand[0], g_rHand[1],
            g_rMoving ? " (moving: angle checks informational)" : "");
    const float scale = Cfg().worldScale;
    stereo::FrameStats s;
    bool haveStereo = stereo::LastStats(s);
    float gameYaw = 0;
    if (!haveStereo) {
        PVR_LOG("INV #%d stereo: no stereo frame yet", g_checkNo);
    } else {
        float d[3] = {s.eyeOrigin[1][0] - s.eyeOrigin[0][0], s.eyeOrigin[1][1] - s.eyeOrigin[0][1],
                      s.eyeOrigin[1][2] - s.eyeOrigin[0][2]};
        float ipd = vm::Len3(d), ipdExp = kIpd * scale;
        // eye right (from left to right eye) must be horizontal-ish rotated by head roll only: check it is
        // perpendicular to the eye forward
        float perp = vm::Dot3(d, s.eyeFwd[0]) / (ipd + 1e-9f);
        float fwdLR = AngDeg(s.eyeFwd[0], s.eyeFwd[1]);
        gameYaw = std::atan2(s.gameFwd[0], s.gameFwd[1]) / kDeg;
        float gamePitch = std::asin(std::fmax(-1.f, std::fmin(1.f, s.gameFwd[2]))) / kDeg;
        float eyeYaw = std::atan2(s.eyeFwd[0][0], s.eyeFwd[0][1]) / kDeg;
        float eyePitch = std::asin(std::fmax(-1.f, std::fmin(1.f, s.eyeFwd[0][2]))) / kDeg;
        // head yaw + = left; game yaw grows clockwise seen from above (Z up) -> expected = game - head
        float yawErr = WrapDeg(eyeYaw - (gameYaw - g_rHead[0]));
        float pitchErr = eyePitch - g_rHead[1];
        float baseUp = s.base[1][2];
        float mid[3] = {(s.eyeOrigin[0][0] + s.eyeOrigin[1][0]) * 0.5f, (s.eyeOrigin[0][1] + s.eyeOrigin[1][1]) * 0.5f,
                        (s.eyeOrigin[0][2] + s.eyeOrigin[1][2]) * 0.5f};
        float off[3] = {mid[0] - s.base[3][0], mid[1] - s.base[3][1], mid[2] - s.base[3][2]};
        float offLen = vm::Len3(off), offExp = vm::Len3(g_rHeadPos) * scale;
        bool okIpd = std::fabs(ipd - ipdExp) < 0.01f * ipdExp && std::fabs(perp) < 0.01f;
        bool okAng = g_rMoving || (std::fabs(yawErr) < 0.5f && std::fabs(pitchErr) < 0.5f && fwdLR < 0.1f);
        bool okPos = std::fabs(offLen - offExp) < 0.01f * scale;
        PVR_LOG("INV #%d stereo %s: frames %llu ipd %.3f (exp %.3f) perp %.4f | fwd L/R %.3f deg | game yaw %.2f pitch "
                "%.2f | eye yaw %.2f pitch %.2f -> yawErr %.2f pitchErr %.2f %s | base up.z %.4f k %.3f | head offset "
                "%.2f (exp %.2f) %s",
                g_checkNo, PF(okIpd && okAng && okPos), s.count, ipd, ipdExp, perp, fwdLR, gameYaw, gamePitch, eyeYaw,
                eyePitch, yawErr, pitchErr, PF(okAng), baseUp, s.k, offLen, offExp, PF(okPos));
    }

    // HUD quad placement (same math SubmitHud uses) relative to the rendered head
    {
        XrPosef head{vm::YawPitchRoll(g_rHead[0] * kDeg, g_rHead[1] * kDeg), {g_rHeadPos[0], g_rHeadPos[1], g_rHeadPos[2]}};
        XrPosef hud = vm::HudPose(head, Cfg().hudDistance, Cfg().hudDown);
        float rel[3] = {hud.position.x - head.position.x, hud.position.y - head.position.y,
                        hud.position.z - head.position.z};
        XrVector3f hf = vm::Forward(head.orientation), qf = vm::Forward(hud.orientation);
        float f1[3] = {hf.x, hf.y, hf.z}, f2[3] = {qf.x, qf.y, qf.z};
        float dist = vm::Len3(rel);
        float distExp = std::sqrt(Cfg().hudDistance * Cfg().hudDistance + Cfg().hudDown * Cfg().hudDown);
        float along = vm::Dot3(rel, f1);
        bool ok = std::fabs(dist - distExp) < 1e-3f && std::fabs(along - Cfg().hudDistance) < 1e-3f && AngDeg(f1, f2) < 0.01f;
        PVR_LOG("INV #%d hudpose %s: dist %.4f (exp %.4f) along-gaze %.4f facing %.3f deg", g_checkNo, PF(ok), dist, distExp,
                along, AngDeg(f1, f2));
    }

    // HUD layer content: where is there anything opaque?
    if (hudSrv) {
        ID3D11Resource *r = nullptr;
        hudSrv->GetResource(&r);
        ID3D11Texture2D *t = nullptr;
        if (r) r->QueryInterface(IID_PPV_ARGS(&t)), r->Release();
        Pix p = Read(t);
        if (t) t->Release();
        if (p.ok) {
            size_t n = 0, rgb = 0;
            UINT x0 = p.w, y0 = p.h, x1 = 0, y1 = 0;
            for (UINT y = 0; y < p.h; ++y)
                for (UINT x = 0; x < p.w; ++x) {
                    if (p.px[(size_t)y * p.w + x] & 0xffffffu) ++rgb;
                    if ((p.px[(size_t)y * p.w + x] >> 24) > 8) {
                        ++n;
                        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                    }
                }
            if (n)
                PVR_LOG("INV #%d hudlayer: %ux%u rgb %.2f%% coverage %.2f%% bbox x %.3f-%.3f y %.3f-%.3f", g_checkNo, p.w, p.h,
                        100.0 * rgb / ((double)p.w * p.h), 100.0 * n / ((double)p.w * p.h), (float)x0 / p.w, (float)x1 / p.w, (float)y0 / p.h, (float)y1 / p.h);
            else
                PVR_LOG("INV #%d hudlayer: %ux%u fully transparent (rgb nonzero %.2f%%)", g_checkNo, p.w, p.h,
                        100.0 * rgb / ((double)p.w * p.h));
        } else {
            PVR_LOG("INV #%d hudlayer: unreadable", g_checkNo);
        }
    } else {
        PVR_LOG("INV #%d hudlayer: none this frame", g_checkNo);
    }

    // eye images: brightness and how different the two eyes are (0 = identical -> no stereo)
    {
        Pix a = Read(eye0), b = Read(eye1);
        if (a.ok && b.ok && a.w == b.w && a.h == b.h) {
            double la = 0, lb = 0;
            size_t diff = 0, step = 7;
            size_t cnt = 0;
            for (size_t i = 0; i < a.px.size(); i += step, ++cnt) {
                float x = Luma(a.px[i]), y = Luma(b.px[i]);
                la += x, lb += y;
                if (std::fabs(x - y) > 0.06f) ++diff;
            }
            float dfrac = (float)diff / cnt;
            bool ok = la / cnt > 0.01 && lb / cnt > 0.01 && dfrac > 0.002f && dfrac < 0.6f;
            PVR_LOG("INV #%d eyes %s: %ux%u luma L %.3f R %.3f differing %.1f%%", g_checkNo, PF(ok), a.w, a.h, la / cnt,
                    lb / cnt, 100.0f * dfrac);
        } else {
            PVR_LOG("INV #%d eyes: unreadable or missing", g_checkNo);
        }
    }

    // controller aim (weapon tick): yaw/pitch the weapons used vs the scripted hand
    {
        aim::AimStats a = aim::LastAim();
        if (!Cfg().controllerAim) {
            PVR_LOG("INV #%d aim: disabled", g_checkNo);
        } else if (a.count == 0) {
            PVR_LOG("INV #%d aim: weapon tick never ran", g_checkNo);
        } else {
            float yawDeg = a.yaw / kDeg, pitchDeg = a.pitch / kDeg;
            // game yaw from the stereo base (horizon-level body yaw)
            float yawErr = WrapDeg(yawDeg - (gameYaw - g_rHand[0]));
            float pitchErr = pitchDeg - (-g_rHand[1]);
            bool fresh = a.count != g_aimCountAtCheck;
            bool ok = g_rMoving || (std::fabs(yawErr) < 0.5f && std::fabs(pitchErr) < 0.5f && fresh);
            PVR_LOG("INV #%d aim %s: ticks %llu%s aim yaw %.2f pitch %.2f (actor yaw %.2f pitch %.2f) -> yawErr %.2f "
                    "pitchErr %.2f",
                    g_checkNo, PF(ok), a.count, fresh ? "" : " (STALE)", yawDeg, pitchDeg, a.actorYaw / kDeg,
                    a.actorPitch / kDeg, yawErr, pitchErr);
            g_aimCountAtCheck = a.count;
        }
    }
}

// ---------------- per-frame ----------------
void Frame(ID3D11ShaderResourceView *hud, ID3D11Texture2D *eye0, ID3D11Texture2D *eye1, XrView views[2]) {
    double now = Now();
    if (!g_init) {
        Load();
        g_clock0 = now;
        g_hand.to[0] = g_hand.from[0] = 0;
    }

    // checks against the frame just rendered (only meaningful once the world is being drawn)
    stereo::FrameStats st;
    bool world = stereo::LastStats(st);
    if (Cfg().checkEvery > 0 && world && now - g_lastAuto >= Cfg().checkEvery) {
        g_lastAuto = now;
        if (!g_checkPending) g_checkPending = true, g_checkLabel = "auto";
    }
    if (g_checkPending && world) {
        RunChecks(hud, eye0, eye1);
        g_checkPending = false;
    }

    // script
    if (g_waitWorld && world) {
        g_waitWorld = false;
        g_clock0 = now;
        PVR_LOG("fake: world running, script clock reset");
    }
    while (!g_waitWorld && g_next < g_cmds.size() && now - g_clock0 >= g_cmds[g_next].t) Exec(g_cmds[g_next++], now);

    // head
    float hd[3], hp[3], hn[3];
    g_head.Eval(now, hd);
    g_headPos.Eval(now, hp);
    g_hand.Eval(now, hn);
    for (int i = 0; i < 3; ++i) g_rHead[i] = hd[i], g_rHeadPos[i] = hp[i], g_rHand[i] = hn[i];
    g_rMoving = g_head.Moving(now) || g_headPos.Moving(now) || g_hand.Moving(now);

    XrPosef head{vm::YawPitchRoll(hd[0] * kDeg, hd[1] * kDeg), {hp[0], hp[1], hp[2]}};
    for (int e = 0; e < 2; ++e) {
        XrPosef off{{0, 0, 0, 1}, {e == 0 ? -kIpd * 0.5f : kIpd * 0.5f, 0, 0}};
        views[e] = {XR_TYPE_VIEW};
        views[e].pose = vm::Compose(head, off);
        // headset-like asymmetric fov (outer side wider)
        views[e].fov = e == 0 ? XrFovf{-0.90f, 0.78f, 0.85f, -0.90f} : XrFovf{-0.78f, 0.90f, 0.85f, -0.90f};
    }

    // hands: positioned relative to the head's yaw, oriented absolutely by the script
    XrQuaternionf bodyYaw = vm::YawPitchRoll(hd[0] * kDeg, 0);
    for (int side = 0; side < 2; ++side) {
        XrVector3f rel = vm::Rotate(bodyYaw, {side ? 0.2f : -0.2f, -0.35f, -0.35f});
        XrPosef p{side ? vm::YawPitchRoll(hn[0] * kDeg, hn[1] * kDeg) : bodyYaw,
                  {hp[0] + rel.x, hp[1] + rel.y, hp[2] + rel.z}};
        xr::SetHandPose(side, &p);      // aim
        xr::SetHandPose(side + 2, &p);  // grip
    }

    // virtual pad
    PadState ps;
    ps.connected = true;
    for (int b = 0; b < PB_COUNT; ++b) ps.buttons[b] = now < g_btnUntil[b];
    for (int a = 0; a < PA_COUNT; ++a) {
        float v = now < g_axisUntil[a] ? g_axisVal[a] : 0.0f;
        v = std::fmax(-1.0f, std::fmin(1.0f, v));
        ps.axes[a] = (int16_t)std::lround(v * 32767.0f);
    }
    ps.navX = ps.axes[PA_LEFTX] / 32767.0f, ps.navY = -ps.axes[PA_LEFTY] / 32767.0f;
    PadPublish(ps);
}

}  // namespace pvr::fake
