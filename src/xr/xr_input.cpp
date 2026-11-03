// OpenXR actions for the Steam Frame controllers -> virtual gamepad state.
// The Frame pair is a split gamepad: left = d-pad + View, right = ABXY + Menu,
// both = bumper, trigger, grip, clickable stick. Mapped 1:1 onto an SDL controller.
#include "xr/xr_input.h"

#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "common/log.h"
#include "input/pad_state.h"

namespace pvr::xr {

enum ActKind { K_BOOL, K_FLOAT, K_VEC2, K_POSE };
struct Act {
    const char *name;
    ActKind kind;
    XrAction action = XR_NULL_HANDLE;
};

enum {
    A_A, A_B, A_X, A_Y, A_MENU, A_VIEW, A_DUP, A_DDOWN, A_DLEFT, A_DRIGHT, A_LBUMP, A_RBUMP, A_LCLICK, A_RCLICK,
    A_LTRIG, A_RTRIG, A_LGRIP, A_RGRIP, A_LSTICK, A_RSTICK, A_AIM_L, A_AIM_R, A_GRIP_L, A_GRIP_R, A_COUNT
};
static Act g_acts[A_COUNT] = {
    {"btn_a", K_BOOL},       {"btn_b", K_BOOL},        {"btn_x", K_BOOL},        {"btn_y", K_BOOL},
    {"btn_menu", K_BOOL},    {"btn_view", K_BOOL},     {"dpad_up", K_BOOL},      {"dpad_down", K_BOOL},
    {"dpad_left", K_BOOL},   {"dpad_right", K_BOOL},   {"bumper_left", K_BOOL},  {"bumper_right", K_BOOL},
    {"stick_click_left", K_BOOL}, {"stick_click_right", K_BOOL}, {"trigger_left", K_FLOAT},
    {"trigger_right", K_FLOAT}, {"grip_left", K_FLOAT}, {"grip_right", K_FLOAT}, {"stick_left", K_VEC2},
    {"stick_right", K_VEC2}, {"aim_left", K_POSE},     {"aim_right", K_POSE},    {"grip_pose_left", K_POSE},
    {"grip_pose_right", K_POSE},
};

static XrActionSet g_set = XR_NULL_HANDLE;
static XrSpace g_poseSpace[4] = {};
static XrPosef g_hand[4];
static bool g_handValid[4] = {};
static std::mutex g_handMtx;
static XrInstance g_inst = XR_NULL_HANDLE;
static bool g_ready = false;

// candidate component paths per action; first one the runtime accepts wins
struct Cand {
    int act;
    std::vector<const char *> paths;
};

static XrPath P(const char *s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_inst, s, &p);
    return p;
}

static XrResult Suggest(const char *profile, const std::vector<std::pair<int, std::string>> &b) {
    std::vector<XrActionSuggestedBinding> sb;
    for (auto &[a, path] : b) sb.push_back({g_acts[a].action, P(path.c_str())});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = P(profile);
    s.countSuggestedBindings = (uint32_t)sb.size();
    s.suggestedBindings = sb.data();
    return xrSuggestInteractionProfileBindings(g_inst, &s);
}

// Builds the largest accepted binding set by testing candidates one at a time
// (a single bad path makes the whole suggestion fail).
static void SuggestValidated(const char *profile, const std::vector<Cand> &cands) {
    std::vector<std::pair<int, std::string>> ok;
    std::string rejected;
    for (auto &c : cands) {
        bool found = false;
        for (auto *path : c.paths) {
            auto test = ok;
            test.push_back({c.act, path});
            if (XR_SUCCEEDED(Suggest(profile, test))) {
                ok = test;
                found = true;
                break;
            }
        }
        if (!found) rejected += std::string(" ") + g_acts[c.act].name;
    }
    XrResult r = ok.empty() ? XR_ERROR_PATH_UNSUPPORTED : Suggest(profile, ok);
    PVR_LOG("bindings %s: %zu accepted (r=%d)%s%s", profile, ok.size(), (int)r, rejected.empty() ? "" : "; rejected:",
            rejected.c_str());
}

bool InputInit(XrInstance inst, XrSession sess) {
    g_inst = inst;
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "gameplay");
    strcpy_s(si.localizedActionSetName, "Gameplay");
    if (XR_FAILED(xrCreateActionSet(inst, &si, &g_set))) return false;
    for (auto &a : g_acts) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(ai.actionName, a.name);
        strcpy_s(ai.localizedActionName, a.name);
        ai.actionType = a.kind == K_BOOL    ? XR_ACTION_TYPE_BOOLEAN_INPUT
                        : a.kind == K_FLOAT ? XR_ACTION_TYPE_FLOAT_INPUT
                        : a.kind == K_VEC2  ? XR_ACTION_TYPE_VECTOR2F_INPUT
                                            : XR_ACTION_TYPE_POSE_INPUT;
        if (XR_FAILED(xrCreateAction(g_set, &ai, &a.action))) {
            PVR_LOG("xrCreateAction %s failed", a.name);
            return false;
        }
    }

    const std::vector<Cand> frame = {
        {A_A, {"/user/hand/right/input/a/click"}},
        {A_B, {"/user/hand/right/input/b/click"}},
        {A_X, {"/user/hand/right/input/x/click"}},
        {A_Y, {"/user/hand/right/input/y/click"}},
        {A_MENU, {"/user/hand/right/input/menu/click"}},
        {A_VIEW, {"/user/hand/left/input/view/click"}},
        {A_DUP, {"/user/hand/left/input/dpad_up/click"}},
        {A_DDOWN, {"/user/hand/left/input/dpad_down/click"}},
        {A_DLEFT, {"/user/hand/left/input/dpad_left/click"}},
        {A_DRIGHT, {"/user/hand/left/input/dpad_right/click"}},
        {A_LBUMP, {"/user/hand/left/input/bumper/click", "/user/hand/left/input/shoulder/click"}},
        {A_RBUMP, {"/user/hand/right/input/bumper/click", "/user/hand/right/input/shoulder/click"}},
        {A_LCLICK, {"/user/hand/left/input/thumbstick/click"}},
        {A_RCLICK, {"/user/hand/right/input/thumbstick/click"}},
        {A_LTRIG, {"/user/hand/left/input/trigger/value"}},
        {A_RTRIG, {"/user/hand/right/input/trigger/value"}},
        {A_LGRIP, {"/user/hand/left/input/grip/value", "/user/hand/left/input/squeeze/value"}},
        {A_RGRIP, {"/user/hand/right/input/grip/value", "/user/hand/right/input/squeeze/value"}},
        {A_LSTICK, {"/user/hand/left/input/thumbstick"}},
        {A_RSTICK, {"/user/hand/right/input/thumbstick"}},
        {A_AIM_L, {"/user/hand/left/input/aim/pose"}},
        {A_AIM_R, {"/user/hand/right/input/aim/pose"}},
        {A_GRIP_L, {"/user/hand/left/input/grip/pose"}},
        {A_GRIP_R, {"/user/hand/right/input/grip/pose"}},
    };
    SuggestValidated("/interaction_profiles/valve/frame_controller_valve", frame);

    // fallbacks for other controllers (fewer buttons)
    const std::vector<Cand> touch = {
        {A_A, {"/user/hand/right/input/a/click"}},          {A_B, {"/user/hand/right/input/b/click"}},
        {A_X, {"/user/hand/left/input/x/click"}},           {A_Y, {"/user/hand/left/input/y/click"}},
        {A_MENU, {"/user/hand/left/input/menu/click"}},     {A_LCLICK, {"/user/hand/left/input/thumbstick/click"}},
        {A_RCLICK, {"/user/hand/right/input/thumbstick/click"}},
        {A_LTRIG, {"/user/hand/left/input/trigger/value"}}, {A_RTRIG, {"/user/hand/right/input/trigger/value"}},
        {A_LGRIP, {"/user/hand/left/input/squeeze/value"}}, {A_RGRIP, {"/user/hand/right/input/squeeze/value"}},
        {A_LSTICK, {"/user/hand/left/input/thumbstick"}},   {A_RSTICK, {"/user/hand/right/input/thumbstick"}},
        {A_AIM_L, {"/user/hand/left/input/aim/pose"}},      {A_AIM_R, {"/user/hand/right/input/aim/pose"}},
        {A_GRIP_L, {"/user/hand/left/input/grip/pose"}},    {A_GRIP_R, {"/user/hand/right/input/grip/pose"}},
    };
    SuggestValidated("/interaction_profiles/oculus/touch_controller", touch);

    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &g_set;
    XrResult r = xrAttachSessionActionSets(sess, &at);
    PVR_LOG("action sets attached: %d", (int)r);
    g_ready = XR_SUCCEEDED(r);
    // pose spaces: 0 = left aim, 1 = right aim, 2 = left grip, 3 = right grip
    const int poseActs[4] = {A_AIM_L, A_AIM_R, A_GRIP_L, A_GRIP_R};
    for (int i = 0; i < 4; ++i) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.action = g_acts[poseActs[i]].action;
        sc.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(sess, &sc, &g_poseSpace[i]);
    }
    return g_ready;
}

void LocateHands(XrSpace base, XrTime t) {
    std::lock_guard<std::mutex> lk(g_handMtx);
    for (int i = 0; i < 4; ++i) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        g_handValid[i] = g_poseSpace[i] && XR_SUCCEEDED(xrLocateSpace(g_poseSpace[i], base, t, &loc)) &&
                         (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                         (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        if (g_handValid[i]) g_hand[i] = loc.pose;
    }
}

void SetHandPose(int which, const XrPosef *p) {
    if (which < 0 || which > 3) return;
    std::lock_guard<std::mutex> lk(g_handMtx);
    g_handValid[which] = p != nullptr;
    if (p) g_hand[which] = *p;
}

bool HandPose(int which, XrPosef &out) {
    std::lock_guard<std::mutex> lk(g_handMtx);
    if (which < 0 || which > 3 || !g_handValid[which]) return false;
    out = g_hand[which];
    return true;
}

static bool GetB(XrSession s, int a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_acts[a].action;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive && st.currentState;
}
static float GetF(XrSession s, int a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_acts[a].action;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
static XrVector2f GetV(XrSession s, int a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_acts[a].action;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &st)) && st.isActive) return st.currentState;
    return {0, 0};
}

static int16_t Axis(float v) {
    v = v < -1.f ? -1.f : v > 1.f ? 1.f : v;
    return (int16_t)std::lround(v * 32767.0f);
}

void InputSync(XrSession sess, bool focused, float headYaw) {
    if (!g_ready) return;
    PadState ps;
    ps.connected = true;
    if (focused) {
        XrActiveActionSet as{g_set, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &as;
        if (XR_SUCCEEDED(xrSyncActions(sess, &si))) {
            ps.buttons[PB_A] = GetB(sess, A_A);
            ps.buttons[PB_B] = GetB(sess, A_B);
            ps.buttons[PB_X] = GetB(sess, A_X);
            ps.buttons[PB_Y] = GetB(sess, A_Y);
            ps.buttons[PB_START] = GetB(sess, A_MENU);
            ps.buttons[PB_BACK] = GetB(sess, A_VIEW);
            ps.buttons[PB_DPAD_UP] = GetB(sess, A_DUP);
            ps.buttons[PB_DPAD_DOWN] = GetB(sess, A_DDOWN);
            ps.buttons[PB_DPAD_LEFT] = GetB(sess, A_DLEFT);
            ps.buttons[PB_DPAD_RIGHT] = GetB(sess, A_DRIGHT);
            ps.buttons[PB_LSHOULDER] = GetB(sess, A_LBUMP);
            ps.buttons[PB_RSHOULDER] = GetB(sess, A_RBUMP);
            ps.buttons[PB_LSTICK] = GetB(sess, A_LCLICK);
            ps.buttons[PB_RSTICK] = GetB(sess, A_RCLICK);
            // grips -> paddles for now (free for VR-specific binds later)
            ps.buttons[PB_PADDLE1] = GetF(sess, A_RGRIP) > 0.6f;
            ps.buttons[PB_PADDLE2] = GetF(sess, A_LGRIP) > 0.6f;
            XrVector2f l = GetV(sess, A_LSTICK), r = GetV(sess, A_RSTICK);
            // head-relative locomotion: the game moves relative to body yaw; rotate the stick
            // (x right, y forward) by the head's yaw relative to the body (positive = left)
            float c = std::cos(headYaw), s = std::sin(headYaw);
            float mx = l.x * c - l.y * s, my = l.x * s + l.y * c;
            ps.axes[PA_LEFTX] = Axis(mx);
            ps.axes[PA_LEFTY] = Axis(-my);  // SDL: down is positive
            ps.axes[PA_RIGHTX] = Axis(r.x);
            ps.axes[PA_RIGHTY] = 0;  // no stick pitch in VR: the head does the looking
            ps.axes[PA_TRIGGERLEFT] = Axis(GetF(sess, A_LTRIG));
            ps.axes[PA_TRIGGERRIGHT] = Axis(GetF(sess, A_RTRIG));
        }
    }
    PadPublish(ps);
}

}  // namespace pvr::xr
