#pragma once
#include <atomic>
struct ID3D11Device;
struct ID3D11Resource;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11Buffer;
typedef unsigned int UINT;

namespace pvr {
void InstallD3DHooks();
ID3D11Device *GameDevice();
extern std::atomic<int> g_drawCount;  // diagnostics
// Attribute draws to bound render targets between TraceBegin/TraceEnd; returns the RT with most draws.
void TraceBegin(int slot, ID3D11Resource *initial);
ID3D11Resource *TraceEnd(int slot, bool log);
// RT that received the most recent draw in the trace (not ref-counted)
ID3D11Resource *TraceLast(int slot);
// Diagnostics: log state of the next n draws with a tag.
void LogDraws(int n, const char *tag);
// While set, every OMSetRenderTargets binds this RTV (and no depth). Pass nullptr to clear.
void SetRenderTargetRedirect(ID3D11RenderTargetView *rtv);
// While set, binding a view of `from` binds `to` instead (no depth). Pass to = nullptr to clear.
void SetRenderTargetRemap(ID3D11Resource *from, ID3D11RenderTargetView *to);
// While set, every draw uses this pixel shader + constant buffer (slot). Pass nullptr to clear.
void SetPixelShaderOverride(ID3D11PixelShader *ps, ID3D11Buffer *cb, UINT slot);
}  // namespace pvr
