// Route the game's XAudio2 output to the VR headset's audio endpoint (Steam Link on the
// Frame = "Steam Streaming Speakers") instead of the Windows default device.
#include "hooks/audio_hooks.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <MinHook.h>

#include <string>

#include "common/config.h"
#include "common/log.h"

namespace pvr {

using PFN_XAudio2Create = HRESULT(WINAPI *)(void **ppXAudio2, UINT32 flags, UINT32 proc);
using PFN_CreateMastering = HRESULT(STDMETHODCALLTYPE *)(void *self, void **voice, UINT32 ch, UINT32 rate,
                                                        UINT32 flags, LPCWSTR deviceId, const void *fx,
                                                        int category);
static PFN_XAudio2Create o_XAudio2Create = nullptr;
static PFN_CreateMastering o_CreateMastering = nullptr;
static std::wstring g_deviceId;

static std::wstring FindEndpoint(const std::wstring &needle) {
    std::wstring result;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return result;
    IMMDeviceCollection *col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n && result.empty(); ++i) {
            IMMDevice *dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            IPropertyStore *ps = nullptr;
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) &&
                SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                std::wstring name = pv.pwszVal;
                if (name.find(needle) != std::wstring::npos) {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(dev->GetId(&id))) {
                        result = id;
                        CoTaskMemFree(id);
                        PVR_LOG("audio: routing to '%ls'", name.c_str());
                    }
                }
            }
            PropVariantClear(&pv);
            if (ps) ps->Release();
            dev->Release();
        }
        col->Release();
    }
    en->Release();
    return result;
}

static HRESULT STDMETHODCALLTYPE hk_CreateMastering(void *self, void **voice, UINT32 ch, UINT32 rate, UINT32 flags,
                                                   LPCWSTR deviceId, const void *fx, int category) {
    if (g_deviceId.empty() && !Cfg().audioDevice.empty()) g_deviceId = FindEndpoint(Cfg().audioDevice);
    LPCWSTR use = g_deviceId.empty() ? deviceId : g_deviceId.c_str();
    HRESULT hr = o_CreateMastering(self, voice, ch, rate, flags, use, fx, category);
    if (FAILED(hr) && use != deviceId) {
        PVR_LOG("audio: VR endpoint failed (0x%08X), falling back to default", (unsigned)hr);
        hr = o_CreateMastering(self, voice, ch, rate, flags, deviceId, fx, category);
    }
    PVR_LOG("audio: CreateMasteringVoice hr=0x%08X", (unsigned)hr);
    return hr;
}

static HRESULT WINAPI hk_XAudio2Create(void **pp, UINT32 flags, UINT32 proc) {
    HRESULT hr = o_XAudio2Create(pp, flags, proc);
    if (SUCCEEDED(hr) && pp && *pp && !o_CreateMastering) {
        void **vt = *reinterpret_cast<void ***>(*pp);
        MH_CreateHook(vt[7], (void *)hk_CreateMastering, (void **)&o_CreateMastering);  // IXAudio2::CreateMasteringVoice
        MH_EnableHook(vt[7]);
        PVR_LOG("audio: XAudio2 created, mastering hook installed");
    }
    return hr;
}

void InstallAudioHooks() {
    if (Cfg().audioDevice.empty()) return;
    HMODULE xa = LoadLibraryW(L"xaudio2_9redist.dll");
    void *fn = xa ? (void *)GetProcAddress(xa, "XAudio2Create") : nullptr;
    if (!fn) {
        PVR_LOG("audio: XAudio2Create not found");
        return;
    }
    MH_CreateHook(fn, (void *)hk_XAudio2Create, (void **)&o_XAudio2Create);
    MH_EnableHook(fn);
    PVR_LOG("audio: hook installed, target '%ls'", Cfg().audioDevice.c_str());
}

}  // namespace pvr
