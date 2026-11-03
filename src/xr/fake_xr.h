#pragma once
// FakeXR test harness ([Debug] FakeXR=1): no OpenXR. A script drives the head pose, the hand
// poses and the virtual pad; invariant checks are written to the log as "INV ..." lines.
#include <openxr/openxr.h>

struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace pvr::fake {
// Called once per game Present (render thread). First runs any due checks against the frame the
// game just rendered (hud = overlay layer or null, eyes = captured eye images or null), then
// advances the script and fills the eye views (LOCAL space) for the next frame.
void Frame(ID3D11ShaderResourceView *hud, ID3D11Texture2D *eye0, ID3D11Texture2D *eye1, XrView views[2]);
}  // namespace pvr::fake
