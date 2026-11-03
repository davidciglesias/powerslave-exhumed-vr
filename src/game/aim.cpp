// Controller aiming: the weapon tick (anim actions -> hitscan / projectile spawns, scripted
// A_Fire* functions) reads the player actor's rotation. We set that rotation to the right
// controller's aim ray for the duration of the tick and restore it afterwards, so the camera
// (body yaw) is unaffected while every weapon fires along the controller.
#include "game/aim.h"

#include <windows.h>
#include <MinHook.h>

#include <cmath>
#include <cstring>

#include "common/config.h"
#include "common/log.h"
#include "game/game_addr.h"
#include "render/stereo.h"
#include "xr/xr_input.h"

namespace pvr::aim {
using namespace pvr::game;

using PFN_WeaponTick = void(__fastcall *)(void **weapon);
using PFN_GetActor = uint8_t *(__fastcall *)(void *player);
static PFN_WeaponTick o_WeaponTick = nullptr;

// slaveRotation_s at actor+0x30: x = pitch (down positive), y = roll, z = yaw (scripts:
// dir = (sin(yaw)cos(pitch), cos(yaw)cos(pitch), -sin(pitch)))
constexpr int ROT_Pitch = ACT_Rotation + 0, ROT_Yaw = ACT_Rotation + 8;

static AimStats g_aim;
AimStats LastAim() { return g_aim; }

static bool AimAngles(float &yaw, float &pitch) {
    float base[4][3];
    XrPosef p;
    if (!stereo::BaseFrame(base) || !xr::HandPose(1, p)) return false;
    const XrQuaternionf &q = p.orientation;
    // aim forward in tracking space = q * (0,0,-1)
    float d[3] = {-2.0f * (q.x * q.z + q.w * q.y), -2.0f * (q.y * q.z - q.w * q.x),
                  -(1.0f - 2.0f * (q.x * q.x + q.y * q.y))};
    float w[3];
    for (int i = 0; i < 3; ++i) w[i] = d[0] * base[0][i] + d[1] * base[1][i] + d[2] * base[2][i];
    float l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (l < 1e-5f) return false;
    yaw = std::atan2(w[0], w[1]);
    pitch = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, w[2] / l)));
    return true;
}

static volatile unsigned long long g_lastTick = 0;
unsigned long long LastTickMs() { return g_lastTick; }
static void __fastcall hk_WeaponTick(void **weapon) {
    g_lastTick = GetTickCount64();  // the game ticks only while gameplay runs (not in menus or paused)
    uint8_t *actor = nullptr;
    float yaw, pitch;
    if (Cfg().controllerAim && weapon && weapon[0]) actor = reinterpret_cast<PFN_GetActor>(FN_PlayerGetActor)(weapon[0]);
    if (!actor || !AimAngles(yaw, pitch)) return o_WeaponTick(weapon);

    float *rp = reinterpret_cast<float *>(actor + ROT_Pitch);
    float *ry = reinterpret_cast<float *>(actor + ROT_Yaw);
    float sp = *rp, sy = *ry;
    static int s_log = 0;
    if (s_log++ < 3)
        PVR_LOG("aim: actor rot (%.3f %.3f %.3f) render angles (%.3f %.3f %.3f) -> aim yaw %.3f pitch %.3f",
                rp[0], rp[1], rp[2], *reinterpret_cast<float *>(actor + 0x94), *reinterpret_cast<float *>(actor + 0x98),
                *reinterpret_cast<float *>(actor + 0x9c), yaw, pitch);
    *rp = pitch;
    // keep yaw continuous with the actor's own (avoid +-pi jumps for code that interpolates)
    float dy = yaw - sy;
    while (dy > 3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    *ry = sy + dy;
    g_aim.yaw = *ry, g_aim.pitch = pitch, g_aim.actorYaw = sy, g_aim.actorPitch = sp;
    ++g_aim.count;
    o_WeaponTick(weapon);
    *rp = sp;
    *ry = sy;
}

void Install() {
    if (!BuildMatches()) return;
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void *>(FN_WeaponTick), (void *)hk_WeaponTick, (void **)&o_WeaponTick);
    MH_EnableHook(reinterpret_cast<void *>(FN_WeaponTick));
    PVR_LOG("aim: WeaponTick hook %d (controller aim %d)", (int)s, (int)Cfg().controllerAim);
}

}  // namespace pvr::aim
