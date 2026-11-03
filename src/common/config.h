#pragma once
#include <string>

namespace pvr {
struct Config {
    bool xrEnabled = true;
    float screenDistance = 2.0f;   // meters, flat-screen quad
    float screenWidth = 2.4f;      // meters
    bool stereo = true;            // render the world once per eye (projection layer)
    float worldScale = 64.0f;      // game units per meter (eye height 104u ~ 1.63m)
    bool controllerAim = true;     // weapons fire along the right controller
    bool handWeapon = true;        // weapon sprite as a card in the right hand
    float weaponWidth = 0.75f;     // meters, width of the cropped 2D weapon region (0.75 m per full screen width)
    float weaponForward = 0.06f;   // meters along the aim ray from the controller
    float weaponUp = -0.12f;       // meters along controller up (card bottom edge)
    float weaponThickness = 0.04f;  // meters: depth of the voxel extrusion of the sprite
    float weaponVoxel = 8.0f;       // screen pixels (of the 2D weapon image) per voxel
    float weaponEdgeShade = 0.65f;  // brightness of the side walls
    // left hand: cut once out of a weapon's 2D image (the ring's left hand), saved to PowerslaveVR_hand.raw
    bool handEnable = false;
    int handSourceWeapon = 6;
    float handSrc[4] = {0.0f, 0.2813f, 0.5874f, 0.8252f};  // x0 x1 y0 y1 of the 2048 weapon image
    float handWidth = 0.16f, handForward = 0.0f, handUp = -0.08f, handThickness = 0.03f;
    bool handMirror = false;
    float weaponCropX0 = 0.0f, weaponCropX1 = 1.0f, weaponCropY0 = 0.25f, weaponCropY1 = 1.0f;
    bool hudOverlay = true;        // HUD/menus on a head-locked transparent layer
    float hudDistance = 1.2f, hudWidth = 0.8f, hudDown = -0.04f;  // meters (negative down = raised)
    // the bottom of the 2D canvas (status bar, from hudSplit to 1) goes on its own lower panel
    float hudAspect = 0.0f;  // >0: widen the HUD ortho to this canvas aspect (experiment; did not reveal HP)
    bool dumpWeapons = false;    // debug: dump each held weapon's 2D image once (pvr_weapon_N.raw)
    bool dumpLayers = false;     // debug: write the HUD/menu layers to pvr_hud_N.raw / pvr_menu_N.raw (4 x 16 MB)
    bool hudNoRedirect = false;  // debug: HUD drawn into the game's target, then copied to the HUD layer
    float hudOrthoY = 0.0f;     // debug: >0 also widens the HUD ortho vertically by this factor
    float menuWidth = 1.2f;     // m, square menu panel
    // chest panel (health / ammo / air), seen by looking down; follows the body yaw lazily
    bool gauges = true;
    float iconSrc[4] = {0.76f, 0.94f, 0.0f, 0.13f};  // power/compass icon crop of the HUD canvas (x0 x1 y0 y1)
    float iconSize = 0.07f;                         // m, on the chest panel right of the bars (0 = hidden)
    float msgY0 = 0.15f, msgY1 = 0.32f, msgPitch = 12.0f;  // message band of the canvas; deg below the gaze
    float gaugeWidth = 0.22f, gaugeDown = 0.45f, gaugeForward = 0.30f, gaugeTilt = 50.0f, gaugeDeadzone = 25.0f;  // m, m, m, deg, deg
    float hudSplit = 0.70f, barWidth = 1.0f, barPitch = 30.0f, barDistance = 1.0f;  // -, m, deg, m
    bool virtualPad = true;        // feed OpenXR input into SDL gamecontroller
    bool fakeXR = false;           // [Debug] scripted head/hands/pad, no OpenXR (test harness)
    std::wstring fakeScript = L"PowerslaveVR_test.txt";  // relative to the game dir
    float checkEvery = 0.0f;       // seconds between automatic invariant checks (0 = script only)
    std::wstring audioDevice = L"Steam Streaming Speakers";  // substring of endpoint name; empty = Windows default
};
Config &Cfg();
void ConfigLoad(const std::wstring &iniPath);
std::wstring GameDir();
}  // namespace pvr
