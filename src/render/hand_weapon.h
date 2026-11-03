#pragma once
#include <d3d11.h>

namespace pvr::hand {
// Hooks the 2D first-person weapon draw. Needs MH_Initialize().
void Install();
// Set by the stereo renderer around each eye's world draw (-1 = not in a stereo pass).
void SetEye(int eye);
// Draw the captured weapon sprite attached to the right controller as a voxel extrusion (every
// opaque sprite pixel becomes a column of depth [Weapon] Thickness), ray-traced in a box, into the
// eye's scene target, depth-tested against the world. viewProj = row-vector world->clip,
// camPos = the eye's world position.
void DrawCard(ID3D11DeviceContext *ctx, const float viewProj[16], const float camPos[3], ID3D11Texture2D *sceneRT,
              ID3D11DepthStencilView *dsv, bool reversedZ);
}  // namespace pvr::hand
