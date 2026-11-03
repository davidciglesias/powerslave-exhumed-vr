#pragma once
#include <d3d11.h>

namespace pvr::overlay {
struct Out {
    ID3D11ShaderResourceView *srv = nullptr;  // null if the layer was not drawn this frame
    UINT w = 0, h = 0;
};
// From the end of the stereo world pass until Present, draws aimed at the scene target
// (menus, messages) are remapped into the transparent "menu" layer.
void Begin(ID3D11DeviceContext *ctx, ID3D11Texture2D *sceneRT);
// HUD canvas layer (status bar), drawn inside the world pass: clears it once per frame and
// returns its RTV, sized like `like`.
ID3D11RenderTargetView *Prepare(ID3D11DeviceContext *ctx, ID3D11Texture2D *like);
// Menu layer for 2D screens drawn inside the world pass (automap): clears it once per frame, returns its RTV.
ID3D11RenderTargetView *PrepareMenu(ID3D11DeviceContext *ctx, ID3D11Texture2D *like);
// Called at Present: stops the remap and returns this frame's layers.
void End(Out &hud, Out &menu);
// Diagnostics: the menu layer texture and the scene target being remapped into it.
ID3D11Texture2D *Target();
ID3D11Texture2D *SceneTarget();
}  // namespace pvr::overlay
