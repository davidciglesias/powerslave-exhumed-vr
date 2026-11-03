// Virtual SDL game controller fed by OpenXR (see xr/xr_input.cpp).
// The game opens controllers on SDL_CONTROLLERDEVICEADDED, takes buttons from
// SDL_CONTROLLERBUTTONDOWN/UP events and polls sticks/triggers via SDL_GameControllerGetAxis.
// We append one fake device after the real ones and synthesize its events in SDL_PollEvent.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <deque>

#include "common/config.h"
#include "common/log.h"
#include "input/menu_nav.h"
#include "input/pad_state.h"

namespace pvr {
static std::mutex g_padMtx;
static PadState g_pad;
static bool g_menuMode = false;
static MenuNav g_nav;
void PadSetMenuMode(bool on) {
    std::lock_guard<std::mutex> lk(g_padMtx);
    if (on == g_menuMode) return;
    static int s_logs = 0;
    if (s_logs < 60) ++s_logs, PVR_LOG("menu nav %s", on ? "on" : "off");
    g_menuMode = on;
    g_nav.Reset();
}
void PadPublish(const PadState &in) {
    PadState s = in;
    std::lock_guard<std::mutex> lk(g_padMtx);
    if (g_menuMode) {
        static const PadButton kBtn[4] = {PB_DPAD_UP, PB_DPAD_DOWN, PB_DPAD_LEFT, PB_DPAD_RIGHT};
        int d = g_nav.Update(s.navX, s.navY, GetTickCount64());
        if (d != MenuNav::NONE) s.buttons[kBtn[d]] = true;
        s.axes[PA_LEFTX] = s.axes[PA_LEFTY] = 0;
    }
    g_pad = s;
}
PadState PadSnapshot() {
    std::lock_guard<std::mutex> lk(g_padMtx);
    return g_pad;
}
}  // namespace pvr

using namespace pvr;

// --- minimal SDL ABI (SDL 2.x) ---
using SDL_bool = int;
using SDL_JoystickID = int32_t;
struct SDL_GameController;
struct SDL_Joystick;
struct SDL_Haptic;
union SDL_Event {
    uint32_t type;
    uint8_t padding[56];
};
struct CtrlDeviceEvent { uint32_t type, timestamp; int32_t which; };
struct CtrlButtonEvent { uint32_t type, timestamp; SDL_JoystickID which; uint8_t button, state, p1, p2; };
struct CtrlAxisEvent { uint32_t type, timestamp; SDL_JoystickID which; uint8_t axis, p1, p2, p3; int16_t value; uint16_t p4; };
enum : uint32_t {
    SDL_CONTROLLERAXISMOTION = 0x650,
    SDL_CONTROLLERBUTTONDOWN = 0x651,
    SDL_CONTROLLERBUTTONUP = 0x652,
    SDL_CONTROLLERDEVICEADDED = 0x653,
};

// real SDL2 functions
template <typename T>
static T Real(const char *name) {
    static HMODULE mod = GetModuleHandleW(L"SDL2_real.dll");
    if (!mod) mod = LoadLibraryW(L"SDL2_real.dll");
    return reinterpret_cast<T>(GetProcAddress(mod, name));
}
#define REAL(ret, name, ...)                                     \
    using PFN_##name = ret(__cdecl *)(__VA_ARGS__);              \
    static PFN_##name r_##name() {                               \
        static PFN_##name f = Real<PFN_##name>(#name);           \
        return f;                                                \
    }
REAL(int, SDL_NumJoysticks, void)
REAL(SDL_bool, SDL_IsGameController, int)
REAL(SDL_GameController *, SDL_GameControllerOpen, int)
REAL(void, SDL_GameControllerClose, SDL_GameController *)
REAL(SDL_bool, SDL_GameControllerGetAttached, SDL_GameController *)
REAL(int16_t, SDL_GameControllerGetAxis, SDL_GameController *, int)
REAL(SDL_Joystick *, SDL_GameControllerGetJoystick, SDL_GameController *)
REAL(uint16_t, SDL_GameControllerGetProduct, SDL_GameController *)
REAL(uint16_t, SDL_GameControllerGetVendor, SDL_GameController *)
REAL(const char *, SDL_GameControllerName, SDL_GameController *)
REAL(SDL_bool, SDL_GameControllerHasSensor, SDL_GameController *, int)
REAL(int, SDL_GameControllerSetSensorEnabled, SDL_GameController *, int, SDL_bool)
REAL(int, SDL_GameControllerGetSensorData, SDL_GameController *, int, float *, int)
REAL(SDL_JoystickID, SDL_JoystickGetDeviceInstanceID, int)
REAL(SDL_JoystickID, SDL_JoystickInstanceID, SDL_Joystick *)
REAL(int, SDL_JoystickIsHaptic, SDL_Joystick *)
REAL(SDL_Haptic *, SDL_HapticOpenFromJoystick, SDL_Joystick *)
REAL(int, SDL_PollEvent, SDL_Event *)
REAL(uint32_t, SDL_GetTicks, void)

static uint8_t g_fakeCtrl[64], g_fakeJoy[64];
#define FAKE_GC reinterpret_cast<SDL_GameController *>(g_fakeCtrl)
#define FAKE_JOY reinterpret_cast<SDL_Joystick *>(g_fakeJoy)
static const SDL_JoystickID kFakeId = 0x5056;  // 'PV'

static bool Enabled() { return Cfg().virtualPad; }
static int FakeIndex() { return r_SDL_NumJoysticks()(); }
static bool IsFakeIndex(int i) { return Enabled() && i == FakeIndex(); }

extern "C" {
int vp_SDL_NumJoysticks(void) { return r_SDL_NumJoysticks()() + (Enabled() ? 1 : 0); }
SDL_bool vp_SDL_IsGameController(int i) { return IsFakeIndex(i) ? 1 : r_SDL_IsGameController()(i); }
SDL_GameController *vp_SDL_GameControllerOpen(int i) {
    if (IsFakeIndex(i)) {
        PVR_LOG("game opened virtual pad (index %d)", i);
        return FAKE_GC;
    }
    SDL_GameController *g = r_SDL_GameControllerOpen()(i);
    PVR_LOG("game opened real controller index %d: %p '%s' id=%d", i, (void *)g,
            g ? r_SDL_GameControllerName()(g) : "", r_SDL_JoystickGetDeviceInstanceID()(i));
    return g;
}
void vp_SDL_GameControllerClose(SDL_GameController *g) {
    if (g != FAKE_GC) r_SDL_GameControllerClose()(g);
}
SDL_bool vp_SDL_GameControllerGetAttached(SDL_GameController *g) {
    return g == FAKE_GC ? 1 : r_SDL_GameControllerGetAttached()(g);
}
int16_t vp_SDL_GameControllerGetAxis(SDL_GameController *g, int axis) {
    if (g != FAKE_GC) return r_SDL_GameControllerGetAxis()(g, axis);
    if (axis < 0 || axis >= PA_COUNT) return 0;
    return PadSnapshot().axes[axis];
}
SDL_Joystick *vp_SDL_GameControllerGetJoystick(SDL_GameController *g) {
    return g == FAKE_GC ? FAKE_JOY : r_SDL_GameControllerGetJoystick()(g);
}
uint16_t vp_SDL_GameControllerGetProduct(SDL_GameController *g) {
    return g == FAKE_GC ? 0x028E : r_SDL_GameControllerGetProduct()(g);  // Xbox 360 pad
}
uint16_t vp_SDL_GameControllerGetVendor(SDL_GameController *g) {
    return g == FAKE_GC ? 0x045E : r_SDL_GameControllerGetVendor()(g);  // Microsoft
}
const char *vp_SDL_GameControllerName(SDL_GameController *g) {
    return g == FAKE_GC ? "PowerslaveVR Steam Frame" : r_SDL_GameControllerName()(g);
}
SDL_bool vp_SDL_GameControllerHasSensor(SDL_GameController *g, int t) {
    return g == FAKE_GC ? 0 : r_SDL_GameControllerHasSensor()(g, t);
}
int vp_SDL_GameControllerSetSensorEnabled(SDL_GameController *g, int t, SDL_bool e) {
    return g == FAKE_GC ? -1 : r_SDL_GameControllerSetSensorEnabled()(g, t, e);
}
int vp_SDL_GameControllerGetSensorData(SDL_GameController *g, int t, float *d, int n) {
    return g == FAKE_GC ? -1 : r_SDL_GameControllerGetSensorData()(g, t, d, n);
}
SDL_JoystickID vp_SDL_JoystickGetDeviceInstanceID(int i) {
    return IsFakeIndex(i) ? kFakeId : r_SDL_JoystickGetDeviceInstanceID()(i);
}
SDL_JoystickID vp_SDL_JoystickInstanceID(SDL_Joystick *j) {
    return j == FAKE_JOY ? kFakeId : r_SDL_JoystickInstanceID()(j);
}
int vp_SDL_JoystickIsHaptic(SDL_Joystick *j) { return j == FAKE_JOY ? 0 : r_SDL_JoystickIsHaptic()(j); }
SDL_Haptic *vp_SDL_HapticOpenFromJoystick(SDL_Joystick *j) {
    return j == FAKE_JOY ? nullptr : r_SDL_HapticOpenFromJoystick()(j);
}

// Real events first; then synthetic events for the virtual pad, generated by diffing
// the latest XR pad snapshot against what we last reported to the game.
int vp_SDL_PollEvent(SDL_Event *ev) {
    static bool s_announced = false;
    static PadState s_sent;
    static std::deque<SDL_Event> s_q;

    if (!ev) return r_SDL_PollEvent()(nullptr) || !s_q.empty();
    if (r_SDL_PollEvent()(ev)) {
        if (ev->type >= 0x650 && ev->type <= 0x655) {
            static int s_n = 0;
            auto *be = reinterpret_cast<CtrlButtonEvent *>(ev);
            if (ev->type != SDL_CONTROLLERAXISMOTION && s_n++ < 200)
                PVR_LOG("real ctrl event 0x%X which=%d button=%d state=%d", ev->type, be->which, be->button, be->state);
        }
        return 1;
    }
    if (!Enabled()) return 0;

    if (s_q.empty()) {
        uint32_t now = r_SDL_GetTicks()();
        if (!s_announced) {
            SDL_Event e{};
            auto *d = reinterpret_cast<CtrlDeviceEvent *>(&e);
            d->type = SDL_CONTROLLERDEVICEADDED;
            d->timestamp = now;
            d->which = FakeIndex();
            s_q.push_back(e);
            s_announced = true;
            PVR_LOG("announced virtual pad at index %d", d->which);
        }
        PadState cur = PadSnapshot();
        for (int b = 0; b < PB_COUNT; ++b) {
            if (cur.buttons[b] == s_sent.buttons[b]) continue;
            SDL_Event e{};
            auto *be = reinterpret_cast<CtrlButtonEvent *>(&e);
            be->type = cur.buttons[b] ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
            be->timestamp = now;
            be->which = kFakeId;
            be->button = (uint8_t)b;
            be->state = cur.buttons[b] ? 1 : 0;
            s_q.push_back(e);
        }
        for (int a = 0; a < PA_COUNT; ++a) {
            if (cur.axes[a] == s_sent.axes[a]) continue;
            SDL_Event e{};
            auto *ae = reinterpret_cast<CtrlAxisEvent *>(&e);
            ae->type = SDL_CONTROLLERAXISMOTION;
            ae->timestamp = now;
            ae->which = kFakeId;
            ae->axis = (uint8_t)a;
            ae->value = cur.axes[a];
            s_q.push_back(e);
        }
        s_sent = cur;
    }
    if (s_q.empty()) return 0;
    *ev = s_q.front();
    if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
        auto *be = reinterpret_cast<CtrlButtonEvent *>(ev);
        PVR_LOG("virtual button %d down", be->button);
    }
    s_q.pop_front();
    return 1;
}
}  // extern "C"
