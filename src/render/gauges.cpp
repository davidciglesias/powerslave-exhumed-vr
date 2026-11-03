#include "render/gauges.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "hooks/d3d_hooks.h"
#include "render/blit.h"

namespace pvr::gauges {

// One gauge per half of the texture: a frame, a fill bar (left 62 %) and a 3-digit
// seven-segment number (right). Alpha is straight (unpremultiplied).
static const char *kShader = R"(
cbuffer G : register(b0) { float4 col; float4 p; float4 crop; float4 bar; };  // p: fill 0..1, value (-1 = none), blink, have canvas
Texture2D canvas : register(t0);
SamplerState lin : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float Seg(float2 q, float2 a, float2 b, float w) {
    float2 qa = q - a, ba = b - a;
    float h = saturate(dot(qa, ba) / dot(ba, ba));
    return length(qa - ba * h) < w ? 1.0 : 0.0;
}
static const uint kSeg[11] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x40};  // 10 = '-'
float Digit(float2 q, uint d) {  // q in [0,1]^2, y up
    if (any(q < 0) || any(q > 1)) return 0;
    uint m = kSeg[d];
    float w = 0.085, r = 0;
    float2 A = float2(0.2, 0.92), B = float2(0.8, 0.92), C = float2(0.8, 0.5), D = float2(0.8, 0.08);
    float2 E = float2(0.2, 0.08), F = float2(0.2, 0.5);
    if (m & 1) r = max(r, Seg(q, A, B, w));
    if (m & 2) r = max(r, Seg(q, B, C, w));
    if (m & 4) r = max(r, Seg(q, C, D, w));
    if (m & 8) r = max(r, Seg(q, E, D, w));
    if (m & 16) r = max(r, Seg(q, E, F, w));
    if (m & 32) r = max(r, Seg(q, F, A, w));
    if (m & 64) r = max(r, Seg(q, F, C, w));
    return r;
}
// The row reuses the game's own HUD art: the carved frame around the ammo bar, cut out of this
// frame's HUD canvas (t0, crop = canvas uv rect), with our fill drawn into its bar slot (bar = slot
// rect inside the crop, 0..1). Number on the right in the game's gold.
float3 Stone(float2 uv) { return float3(0.36, 0.22, 0.12) * (0.85 + 0.15 * frac(sin(dot(floor(uv * float2(40, 10)), float2(12.9898, 78.233))) * 43758.5453)); }
float4 ps(V i) : SV_Target {
    float2 c = i.uv;
    float4 o = p.w > 0.5 ? canvas.SampleLevel(lin, lerp(crop.xy, crop.zw, c), 0) : float4(Stone(c), 1.0);
    o.a = 1.0;
    if (all(c >= bar.xy) && all(c <= bar.zw)) {
        float2 f = (c - bar.xy) / (bar.zw - bar.xy);
        if (f.y < 0.15) o = float4(col.rgb * 0.25, 1.0);                 // dark top line, as the game's
        else o = f.x <= p.x ? float4(col.rgb, 1.0) : float4(0, 0, 0, 1);  // fill / empty (black)
        if (p.z > 0.5 && f.x <= p.x && f.y >= 0.15) o.rgb = min(1.0, o.rgb * 1.6);
    }
    return o;
}
)";

static ID3D11VertexShader *g_vs = nullptr;
static ID3D11PixelShader *g_ps = nullptr;
static ID3D11Buffer *g_cb = nullptr;
static ID3D11SamplerState *g_samp = nullptr;
static ID3D11BlendState *g_bs = nullptr;
static ID3D11RasterizerState *g_rs = nullptr;
static ID3D11DepthStencilState *g_ds = nullptr;
static ID3D11Texture2D *g_tex = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;
static ID3D11ShaderResourceView *g_srv = nullptr;
static bool g_failed = false;
static const UINT kW = 600, kRow = 128, kRows = 4, kH = kRow * kRows;  // row = crop aspect 700:150

static bool Init() {
    if (g_srv) return true;
    if (g_failed) return false;
    g_failed = true;
    ID3D11Device *dev = GameDevice();
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "pvr_gauges", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "pvr_gauges", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, &err))) {
        PVR_LOG("gauges: shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        return false;
    }
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_ps);
    vb->Release();
    pb->Release();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_samp);
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &g_bs);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dev->CreateDepthStencilState(&dd, &g_ds);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_tex))) return false;
    dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv);
    dev->CreateShaderResourceView(g_tex, nullptr, &g_srv);
    g_failed = !(g_vs && g_ps && g_cb && g_rtv && g_srv);
    if (!g_failed) PVR_LOG("gauges: ready %ux%u", kW, kH);
    return !g_failed;
}

ID3D11ShaderResourceView *Render(ID3D11DeviceContext *ctx, const hudstate::Stats &s, ID3D11ShaderResourceView *canvas,
                                 UINT &w, UINT &h) {
    if (!Init()) return nullptr;
    blit::SavedState st;
    st.Save(ctx);
    ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ctx->OMSetBlendState(g_bs, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(g_ds, 0);
    ctx->RSSetState(g_rs);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetShaderResources(0, 1, &canvas);
    ctx->PSSetSamplers(0, 1, &g_samp);
    struct Row { float r, g, b, fill, value, blink; };
    Row rows[kRows];
    UINT n = 0;
    rows[n++] = {1.0f, 0.125f, 0.125f, s.hpMax > 0 ? (float)s.hp / (float)s.hpMax : 0.0f, (float)(s.hp < 0 ? 0 : s.hp),
                 s.hp <= s.hpMax / 4 ? 1.0f : 0.0f};  // health: the game's red
    rows[n++] = {0.125f, 0.125f, 1.0f, s.ammoMax > 0 ? (float)s.ammo / (float)s.ammoMax : 1.0f,
                 s.ammoMax > 0 ? (float)s.ammo : -1.0f, 0.0f};  // ammo: the game's blue
    if (s.underwater && s.airMax > 0)  // air: cyan, value in percent
        rows[n++] = {0.25f, 0.75f, 0.95f, (float)s.air / (float)s.airMax, (float)(s.air * 100 / s.airMax),
                     s.air * 4 <= s.airMax ? 1.0f : 0.0f};
    for (UINT row = 0; row < n; ++row) {
        const Row &R = rows[row];
        // crop of the game's ammo bar frame in the square 2048 HUD canvas (measured from a dump:
        // frame ~x 300-1000, y 1640-1790; bar slot x 467-861, y 1677-1701)
        float cb[16] = {R.r, R.g, R.b, 1.0f, R.fill, R.value, R.blink, canvas ? 1.0f : 0.0f,
                        300 / 2048.f, 1640 / 2048.f, 1000 / 2048.f, 1790 / 2048.f,
                        (467 - 300) / 700.f, (1676 - 1640) / 150.f, (862 - 300) / 700.f, (1702 - 1640) / 150.f};
        if (cb[4] < 0) cb[4] = 0;
        if (cb[4] > 1) cb[4] = 1;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        memcpy(m.pData, cb, sizeof(cb));
        ctx->Unmap(g_cb, 0);
        D3D11_VIEWPORT vp{0, (float)(row * kRow), (float)kW, (float)kRow, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
    }
    st.Restore(ctx);
    w = kW, h = kRow * n;
    return g_srv;
}
void Dump(ID3D11DeviceContext *ctx, const wchar_t *path) {
    if (!g_tex) return;
    D3D11_TEXTURE2D_DESC d;
    g_tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    if (FAILED(GameDevice()->CreateTexture2D(&d, nullptr, &st))) return;
    ctx->CopyResource(st, g_tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = _wfopen(path, L"wb");
        if (f) {
            for (UINT y = 0; y < d.Height; ++y) fwrite((uint8_t *)m.pData + y * m.RowPitch, 4, d.Width, f);
            fclose(f);
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
}
}  // namespace pvr::gauges
