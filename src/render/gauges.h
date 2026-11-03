#pragma once
#include <d3d11.h>

#include "game/hud_state.h"

namespace pvr::gauges {
// Renders the wrist panel into an internal RGBA texture, one gauge row per value (health, ammo,
// air while underwater) from the top. Returns its SRV (null on failure); w/h = the used area.
// canvas = this frame's HUD canvas (the game's art is cut out of it); may be null.
ID3D11ShaderResourceView *Render(ID3D11DeviceContext *ctx, const hudstate::Stats &s, ID3D11ShaderResourceView *canvas,
                                 UINT &w, UINT &h);
// Diagnostics: write the panel texture as raw RGBA8 (768x512).
void Dump(ID3D11DeviceContext *ctx, const wchar_t *path);
}  // namespace pvr::gauges
