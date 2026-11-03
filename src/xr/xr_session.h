#pragma once
#include <openxr/openxr.h>
struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace pvr::xr {
// Called from the hooked Present on the game's render thread, before the real Present.
// Ends the XR frame the game just rendered, then waits/begins the next one.
void OnPresent(IDXGISwapChain *sc);
bool Running();
// Eye views (LOCAL space) predicted for the frame currently being rendered. False if none.
bool FrameViews(XrView out[2]);
// Copy a rendered eye image (any size/format of the game's targets) for submission at Present.
void CaptureEye(int eye, ID3D11Texture2D *src);
}  // namespace pvr::xr
