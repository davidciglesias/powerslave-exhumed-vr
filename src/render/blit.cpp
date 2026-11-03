#include "render/blit.h"

#include <d3dcompiler.h>

#include <cstring>

#include "common/log.h"
#include "hooks/d3d_hooks.h"

namespace pvr::blit {

static const char *kShader = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target { return float4(t0.Sample(s0, i.uv).rgb, 1); }
float4 psAlpha(V i) : SV_Target {
    float4 c = t0.Sample(s0, i.uv);
    // HUD blending may not write alpha: treat any non-black pixel as covered
    float a = max(c.a, step(0.004, max(c.r, max(c.g, c.b))));
    return float4(c.rgb, a);
}
)";

static ID3D11VertexShader *g_vs = nullptr;
static ID3D11PixelShader *g_ps = nullptr;
static ID3D11PixelShader *g_psAlpha = nullptr;
static ID3D11SamplerState *g_samp = nullptr;
static ID3D11RasterizerState *g_rs = nullptr;
static ID3D11BlendState *g_bs = nullptr;
static ID3D11DepthStencilState *g_ds = nullptr;
static bool g_initFailed = false;

static bool Init(ID3D11Device *dev) {
    if (g_vs) return true;
    if (g_initFailed) return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "pvr_blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "pvr_blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        PVR_LOG("blit: shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        g_initFailed = true;
        return false;
    }
    ID3DBlob *pab = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "pvr_blit", nullptr, nullptr, "psAlpha", "ps_5_0", 0, 0, &pab, &err))) {
        PVR_LOG("blit: shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        g_initFailed = true;
        return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    dev->CreatePixelShader(pab->GetBufferPointer(), pab->GetBufferSize(), nullptr, &g_psAlpha);
    pab->Release();
    vsb->Release();
    psb->Release();
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_samp);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &g_bs);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dev->CreateDepthStencilState(&dd, &g_ds);
    return g_vs && g_ps;
}


bool Blit(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, UINT w, UINT h,
          bool alphaFromColor) {
    if (!Init(GameDevice())) return false;
    SavedState st;
    st.Save(ctx);
    D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs);
    ctx->OMSetBlendState(g_bs, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(g_ds, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *nullvb = nullptr;
    UINT z = 0;
    ctx->IASetVertexBuffers(0, 1, &nullvb, &z, &z);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(alphaFromColor ? g_psAlpha : g_ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullsrv);
    st.Restore(ctx);
    return true;
}

}  // namespace pvr::blit
