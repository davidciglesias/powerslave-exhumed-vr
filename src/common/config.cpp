#include "common/config.h"

#include <windows.h>
#include <cstdlib>

#include "common/log.h"

namespace pvr {
static Config g_cfg;
Config &Cfg() { return g_cfg; }

static float ReadF(const wchar_t *sec, const wchar_t *key, float def, const std::wstring &ini) {
    wchar_t buf[64];
    wchar_t defs[64];
    swprintf_s(defs, L"%g", def);
    GetPrivateProfileStringW(sec, key, defs, buf, 64, ini.c_str());
    return (float)_wtof(buf);
}

void ConfigLoad(const std::wstring &ini) {
    g_cfg.xrEnabled = GetPrivateProfileIntW(L"General", L"EnableXR", 1, ini.c_str()) != 0;
    g_cfg.virtualPad = GetPrivateProfileIntW(L"Input", L"VirtualPad", 1, ini.c_str()) != 0;
    wchar_t ad[256];
    GetPrivateProfileStringW(L"Audio", L"Device", g_cfg.audioDevice.c_str(), ad, 256, ini.c_str());
    g_cfg.audioDevice = ad;
    g_cfg.stereo = GetPrivateProfileIntW(L"Render", L"Stereo", 1, ini.c_str()) != 0;
    g_cfg.worldScale = ReadF(L"Render", L"WorldScale", g_cfg.worldScale, ini);
    g_cfg.controllerAim = GetPrivateProfileIntW(L"Input", L"ControllerAim", 1, ini.c_str()) != 0;
    g_cfg.handWeapon = GetPrivateProfileIntW(L"Weapon", L"InHand", 1, ini.c_str()) != 0;
    g_cfg.weaponWidth = ReadF(L"Weapon", L"Width", g_cfg.weaponWidth, ini);
    g_cfg.weaponThickness = ReadF(L"Weapon", L"Thickness", g_cfg.weaponThickness, ini);
    g_cfg.weaponVoxel = ReadF(L"Weapon", L"Voxel", g_cfg.weaponVoxel, ini);
    g_cfg.weaponEdgeShade = ReadF(L"Weapon", L"EdgeShade", g_cfg.weaponEdgeShade, ini);
    g_cfg.handEnable = GetPrivateProfileIntW(L"Hand", L"Enable", 0, ini.c_str()) != 0;
    g_cfg.handSourceWeapon = GetPrivateProfileIntW(L"Hand", L"SourceWeapon", g_cfg.handSourceWeapon, ini.c_str());
    g_cfg.handSrc[0] = ReadF(L"Hand", L"SrcX0", g_cfg.handSrc[0], ini);
    g_cfg.handSrc[1] = ReadF(L"Hand", L"SrcX1", g_cfg.handSrc[1], ini);
    g_cfg.handSrc[2] = ReadF(L"Hand", L"SrcY0", g_cfg.handSrc[2], ini);
    g_cfg.handSrc[3] = ReadF(L"Hand", L"SrcY1", g_cfg.handSrc[3], ini);
    g_cfg.handWidth = ReadF(L"Hand", L"Width", g_cfg.handWidth, ini);
    g_cfg.handForward = ReadF(L"Hand", L"Forward", g_cfg.handForward, ini);
    g_cfg.handUp = ReadF(L"Hand", L"Up", g_cfg.handUp, ini);
    g_cfg.handThickness = ReadF(L"Hand", L"Thickness", g_cfg.handThickness, ini);
    g_cfg.handMirror = GetPrivateProfileIntW(L"Hand", L"Mirror", 0, ini.c_str()) != 0;
    g_cfg.weaponForward = ReadF(L"Weapon", L"Forward", g_cfg.weaponForward, ini);
    g_cfg.weaponUp = ReadF(L"Weapon", L"Up", g_cfg.weaponUp, ini);
    g_cfg.weaponCropX0 = ReadF(L"Weapon", L"CropX0", g_cfg.weaponCropX0, ini);
    g_cfg.weaponCropX1 = ReadF(L"Weapon", L"CropX1", g_cfg.weaponCropX1, ini);
    g_cfg.weaponCropY0 = ReadF(L"Weapon", L"CropY0", g_cfg.weaponCropY0, ini);
    g_cfg.weaponCropY1 = ReadF(L"Weapon", L"CropY1", g_cfg.weaponCropY1, ini);
    g_cfg.hudOverlay = GetPrivateProfileIntW(L"Hud", L"Overlay", 1, ini.c_str()) != 0;
    g_cfg.hudDistance = ReadF(L"Hud", L"Distance", g_cfg.hudDistance, ini);
    g_cfg.hudWidth = ReadF(L"Hud", L"Width", g_cfg.hudWidth, ini);
    g_cfg.hudDown = ReadF(L"Hud", L"Down", g_cfg.hudDown, ini);
    g_cfg.hudAspect = ReadF(L"Hud", L"Aspect", g_cfg.hudAspect, ini);
    g_cfg.dumpWeapons = GetPrivateProfileIntW(L"Debug", L"DumpWeapons", 0, ini.c_str()) != 0;
    g_cfg.dumpLayers = GetPrivateProfileIntW(L"Debug", L"DumpLayers", 0, ini.c_str()) != 0;
    g_cfg.hudNoRedirect = GetPrivateProfileIntW(L"Debug", L"HudNoRedirect", 0, ini.c_str()) != 0;
    g_cfg.hudOrthoY = ReadF(L"Debug", L"HudOrthoY", g_cfg.hudOrthoY, ini);
    g_cfg.gauges = GetPrivateProfileIntW(L"Gauges", L"Enable", 1, ini.c_str()) != 0;
    g_cfg.gaugeWidth = ReadF(L"Gauges", L"Width", g_cfg.gaugeWidth, ini);
    g_cfg.iconSrc[0] = ReadF(L"Gauges", L"IconX0", g_cfg.iconSrc[0], ini);
    g_cfg.iconSrc[1] = ReadF(L"Gauges", L"IconX1", g_cfg.iconSrc[1], ini);
    g_cfg.iconSrc[2] = ReadF(L"Gauges", L"IconY0", g_cfg.iconSrc[2], ini);
    g_cfg.iconSrc[3] = ReadF(L"Gauges", L"IconY1", g_cfg.iconSrc[3], ini);
    g_cfg.iconSize = ReadF(L"Gauges", L"IconSize", g_cfg.iconSize, ini);
    g_cfg.msgY0 = ReadF(L"Hud", L"MsgY0", g_cfg.msgY0, ini);
    g_cfg.msgY1 = ReadF(L"Hud", L"MsgY1", g_cfg.msgY1, ini);
    g_cfg.msgPitch = ReadF(L"Hud", L"MsgPitch", g_cfg.msgPitch, ini);
    g_cfg.gaugeDown = ReadF(L"Gauges", L"Down", g_cfg.gaugeDown, ini);
    g_cfg.gaugeForward = ReadF(L"Gauges", L"Forward", g_cfg.gaugeForward, ini);
    g_cfg.gaugeTilt = ReadF(L"Gauges", L"Tilt", g_cfg.gaugeTilt, ini);
    g_cfg.gaugeDeadzone = ReadF(L"Gauges", L"Deadzone", g_cfg.gaugeDeadzone, ini);
    g_cfg.menuWidth = ReadF(L"Hud", L"MenuWidth", g_cfg.menuWidth, ini);
    g_cfg.hudSplit = ReadF(L"Hud", L"Split", g_cfg.hudSplit, ini);
    g_cfg.barWidth = ReadF(L"Hud", L"BarWidth", g_cfg.barWidth, ini);
    g_cfg.barPitch = ReadF(L"Hud", L"BarPitch", g_cfg.barPitch, ini);
    g_cfg.barDistance = ReadF(L"Hud", L"BarDistance", g_cfg.barDistance, ini);
    g_cfg.screenDistance = ReadF(L"Screen", L"Distance", g_cfg.screenDistance, ini);
    g_cfg.screenWidth = ReadF(L"Screen", L"Width", g_cfg.screenWidth, ini);
    g_cfg.fakeXR = GetPrivateProfileIntW(L"Debug", L"FakeXR", 0, ini.c_str()) != 0;
    wchar_t sc[260];
    GetPrivateProfileStringW(L"Debug", L"Script", g_cfg.fakeScript.c_str(), sc, 260, ini.c_str());
    g_cfg.fakeScript = sc;
    g_cfg.checkEvery = ReadF(L"Debug", L"CheckEvery", g_cfg.checkEvery, ini);
    if (g_cfg.fakeXR) PVR_LOG("config: FakeXR on, script %ls, checkEvery %.1fs", sc, g_cfg.checkEvery);
    PVR_LOG("config: xr=%d stereo=%d scale=%.1f pad=%d screen=%.2fm@%.2fm", g_cfg.xrEnabled, g_cfg.stereo, g_cfg.worldScale, g_cfg.virtualPad,
            g_cfg.screenWidth, g_cfg.screenDistance);
}

std::wstring GameDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}
}  // namespace pvr
