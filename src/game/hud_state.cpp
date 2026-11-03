#include "game/hud_state.h"

#include <windows.h>
#include <cstdint>
#include <mutex>

#include "common/log.h"
#include "game/game_addr.h"

namespace pvr::hudstate {
using namespace pvr::game;

// The HUD (kexSlaveHudLocal, vtbl[0] 0x1401c08b0) reads, with player = *(hud + 8):
//   actor   = GetActor(player) (player + 0xd8 = actor handle), health = short actor + 0x17a
//   weapon  = int player + 0x8c, ammo = short player + 0xbc + 2 * weapon
//   weapon def = HashFind(*(void**)0x140af3330, &weapon) + 8, max ammo = int def + 4 (0 = no ammo)
//   health per container: game mode (vtbl+0x158 of the object at 0x140af33a0) 1|2 -> 100 else 200;
//   containers = short player + 0x84
//   air = short player + 0x9c (air gauge slides in while actor + 0x16c & 0x400 = underwater)
using PFN_GetActor = uint8_t *(__fastcall *)(void *player);
using PFN_HashFind = uint8_t *(__fastcall *)(void *map, const uint32_t *key);
using PFN_Mode = int(__fastcall *)(void *self);

static std::mutex g_mtx;
static Stats g_stats;
static ULONGLONG g_when = 0;

static bool Read(uint8_t *hud, Stats &s) {
    __try {
        uint8_t *player = *reinterpret_cast<uint8_t **>(hud + 8);
        if (!player) return false;
        if (*reinterpret_cast<void **>(player + 0xd8) == nullptr) return false;
        uint8_t *actor = reinterpret_cast<PFN_GetActor>(FN_PlayerGetActor)(player);
        if (!actor) return false;
        s.hp = *reinterpret_cast<short *>(actor + 0x17a);
        void *gi = reinterpret_cast<void *>(GAME_INSTANCE);
        int mode = reinterpret_cast<PFN_Mode>((*reinterpret_cast<void ***>(gi))[0x158 / 8])(gi);
        int per = (mode == 1 || mode == 2) ? 100 : 200;
        int containers = *reinterpret_cast<short *>(player + 0x84);
        s.hpMax = per * (containers > 0 ? containers : 1);
        s.air = *reinterpret_cast<short *>(player + 0x9c);
        s.underwater = (*reinterpret_cast<uint32_t *>(actor + 0x16c) & 0x400) != 0;
        static int s_airMax = 0;
        if (s.air > s_airMax) s_airMax = s.air;
        s.airMax = s_airMax;
        int w = *reinterpret_cast<int *>(player + 0x8c);
        s.weapon = w;
        s.ammo = -1, s.ammoMax = 0;
        if (w >= 0 && w < 8) {
            uint32_t key = (uint32_t)w;
            uint8_t *e = reinterpret_cast<PFN_HashFind>(FN_WeaponDefFind)(*reinterpret_cast<void **>(WEAPON_DEFS), &key);
            int mx = e ? *reinterpret_cast<int *>(e + 8 + 4) : 0;
            if (mx > 0) s.ammoMax = mx, s.ammo = *reinterpret_cast<short *>(player + 0xbc + w * 2);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Capture(void *hud) {
    Stats s;
    if (!hud || !Read(static_cast<uint8_t *>(hud), s)) return;
    static int s_logs = 0;
    if (s_logs < 12) {
        static Stats s_last;
        if (s.hp != s_last.hp || s.weapon != s_last.weapon || s.underwater != s_last.underwater) {
            ++s_logs;
            PVR_LOG("hudstate: hp %d/%d weapon %d ammo %d/%d air %d/%d underwater %d", s.hp, s.hpMax, s.weapon, s.ammo,
                    s.ammoMax, s.air, s.airMax, (int)s.underwater);
            s_last = s;
        }
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_stats = s;
    g_when = GetTickCount64();
}

bool Get(Stats &out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_when || GetTickCount64() - g_when > 250) return false;
    out = g_stats;
    return true;
}
}  // namespace pvr::hudstate
