// SDL2.dll proxy: every SDL export forwards to SDL2_real.dll via the generated .def.
// On attach we install D3D11/DXGI hooks; OpenXR comes up lazily on first Present.
#include <windows.h>

#include "common/config.h"
#include "common/log.h"
#include "common/version.h"
#include "game/aim.h"
#include "hooks/audio_hooks.h"
#include "hooks/d3d_hooks.h"
#include "render/hand_weapon.h"
#include "render/stereo.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        std::wstring dir = pvr::GameDir();
        pvr::LogInit((dir + L"PowerslaveVR.log").c_str());
        PVR_LOG("PowerSlave Exhumed VR " PVR_VERSION " proxy loaded");
        pvr::ConfigLoad(dir + L"PowerslaveVR.ini");
        if (pvr::Cfg().xrEnabled) {
            pvr::InstallD3DHooks();
            pvr::InstallAudioHooks();
            pvr::stereo::Install();
            pvr::aim::Install();
            pvr::hand::Install();
        }
    }
    return TRUE;
}
