#pragma once
// Addresses for anubis_Shipping_Steam_x64.exe, Steam build 19569414 (PE timestamp 0x689a8448).
// The exe has a fixed image base (no DYNAMIC_BASE), so these are absolute.
// Derived in re/ (Ghidra project + re/notes.md).
#include <cstdint>

namespace pvr::game {
constexpr uint32_t kExpectedTimestamp = 0x689a8448;

// --- render view (kexRenderView-like struct, passed by pointer) ---
constexpr uintptr_t FN_ViewBuildProjection = 0x14006fff0;  // void(view*): proj at +0xe0 from fov/near/far/aspect
constexpr uintptr_t FN_ViewBuildViewMatrix = 0x1400701c0;  // void(view*): view at +0x120 (not used by gameplay path)
constexpr uintptr_t FN_ViewFinalize = 0x140070650;         // void(view*): clip = view*proj at +0x1a0, frustum planes at +0
constexpr uintptr_t FN_ViewToConstants = 0x14006ed20;      // void(view*, consts*): fills per-view constant block
constexpr uintptr_t FN_WeaponDraw = 0x14020fc20;          // void(weapon*): 2D first-person weapon sprite
constexpr uintptr_t FN_WeaponTick = 0x14020fb10;          // void(weapon*): weapon[0] = player; runs fire actions
constexpr uintptr_t FN_SkyPass = 0x1401eb240;            // void(view*): PS1-style 2D sky (actor yaw/pitch + fov)
constexpr uintptr_t FN_Ortho = 0x1400d07f0;                // void(float m[16], l, r, b, t, n, f): 2D ortho (HUD canvas)
constexpr uintptr_t FN_HudDraw = 0x1401c08b0;              // kexSlaveHudLocal vtbl[0](this): status bar etc., called inside DrawWorld after post-process
constexpr uintptr_t FN_DrawWorld = 0x1401eff40;            // void(view*, sector*): world draw (gameplay, from 0x1401b7700)

constexpr uintptr_t G_Renderer = 0x140aeaf90;      // kexRenderer* (vtable +0xc0 = UploadConstants(rcx, cb, data, 0, 0))
constexpr uintptr_t G_ViewConstBuffer = 0x140aeb718;  // constant buffer handle used for view constants
constexpr int RendererUploadVtbl = 0xc0;

// view struct offsets
constexpr int VW_Frustum = 0x000;
constexpr int VW_Origin = 0x0c0;      // vec3
constexpr int VW_Projection = 0x0e0;  // float[16], row-vector convention (m[row*4+col])
constexpr int VW_ViewMatrix = 0x120;  // float[16], world->camera, row vectors, translation in row 3
constexpr int VW_ViewRot = 0x160;  // rotation-only copy of the view matrix
constexpr int VW_Clip = 0x1a0;
constexpr int VW_Sky0 = 0x1e0;     // sprite billboard matrices (yaw-only, built from game camera)
constexpr int VW_Sky1 = 0x220;
constexpr int VW_Quat = 0x3e0;
constexpr int VW_Fov = 0x3f0;
constexpr int VW_Near = 0x3f8;
constexpr int VW_Far = 0x3fc;
constexpr int VW_Size = 0x460;        // bytes touched by the builders (consts block follows on stack)
constexpr int ViewConstantsSize = 0x470;

// --- script-API natives (see re/as_registry.tsv) ---
constexpr uintptr_t GAME_INSTANCE = 0x140af33a0;      // object; vtbl+0x158 = game mode (1|2 -> 100 hp per container)
constexpr uintptr_t WEAPON_DEFS = 0x140af3330;        // holds a pointer to the weapon-def hash map (key = weapon index)
constexpr uintptr_t FN_WeaponDefFind = 0x1401bb560;   // entry*(map*, const uint32* key); def = entry + 8, max ammo = int def + 4
constexpr uintptr_t FN_Player = 0x1401f1260;         // kexSlavePlayer* Player()
constexpr uintptr_t FN_PlayerGetActor = 0x140194b30;  // kexSlavePuppet* (player)
constexpr int ACT_Origin = 0x18;
constexpr int ACT_Rotation = 0x30;

bool BuildMatches();  // checks PE timestamp of the running exe
}  // namespace pvr::game
