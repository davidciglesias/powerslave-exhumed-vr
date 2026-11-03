#pragma once
#include <d3d11.h>

namespace pvr::blit {
// Saves/restores the pipeline state our own draws touch (shaders, IA, RS, OM, PS t0/s0/b0).
struct SavedState {
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11GeometryShader *gs = nullptr;
    ID3D11HullShader *hs = nullptr;
    ID3D11DomainShader *ds = nullptr;
    ID3D11InputLayout *il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ID3D11Buffer *vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ID3D11Buffer *ib = nullptr;
    DXGI_FORMAT ibFmt{};
    UINT ibOffset = 0;
    ID3D11RasterizerState *rs = nullptr;
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView *dsv = nullptr;
    ID3D11BlendState *bs = nullptr;
    float bf[4]{};
    UINT mask = 0;
    ID3D11DepthStencilState *dss = nullptr;
    UINT stencilRef = 0;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11SamplerState *samp = nullptr;
    ID3D11Buffer *pscb = nullptr;

    void Save(ID3D11DeviceContext *c) {
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->IAGetIndexBuffer(&ib, &ibFmt, &ibOffset);
        c->RSGetState(&rs);
        c->RSGetViewports(&nvp, vp);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&bs, bf, &mask);
        c->OMGetDepthStencilState(&dss, &stencilRef);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &samp);
        c->PSGetConstantBuffers(0, 1, &pscb);
    }
    void Restore(ID3D11DeviceContext *c) {
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->IASetIndexBuffer(ib, ibFmt, ibOffset);
        c->RSSetState(rs);
        c->RSSetViewports(nvp, vp);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->OMSetBlendState(bs, bf, mask);
        c->OMSetDepthStencilState(dss, stencilRef);
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetSamplers(0, 1, &samp);
        c->PSSetConstantBuffers(0, 1, &pscb);
        IUnknown *all[] = {vs, ps, gs, hs, ds, il, vb, ib, rs, dsv, bs, dss, srv, samp, pscb};
        for (auto *p : all)
            if (p) p->Release();
        for (auto *p : rtv)
            if (p) p->Release();
    }
};

// Draw `src` (non-MSAA texture with SRV support) stretched over `dst` (full target).
// Saves and restores every piece of D3D11 pipeline state it touches.
bool Blit(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, UINT w, UINT h,
          bool alphaFromColor = false);
}  // namespace pvr::blit
