#include "NRDStereo.h"
#include "Features/NRD.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceReflections.h"
#include "Globals.h"
#include "State.h"
#include "Util.h"
#include "Buffer.h"

namespace NRDStereo {
using NRDRendering::Input;
using NRDRendering::FullSize;
using NRDRendering::ActiveSize;
using NRDRendering::BindEye;
namespace {
    uint32_t eye = 0, diffuseMask = 0, specularMask = 0;
    uint32_t outputFrame = UINT32_MAX;
    bool invalidate = true;
    std::unique_ptr<NRD> rightNRD;
    std::unique_ptr<ScreenSpaceGI> rightGI;
    std::unique_ptr<ScreenSpaceReflections> rightSSR;
    struct CropTexture {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        D3D11_TEXTURE2D_DESC desc{};
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    };
    winrt::com_ptr<ID3D11ComputeShader> cropDepthCS;
    std::array<CropTexture,static_cast<size_t>(Input::Count)> crops;
}
uint32_t Eye() { return eye; }
uint32_t DiffuseMask() { return outputFrame == globals::state->frameCount ? diffuseMask : 0u; }
uint32_t SpecularMask() { return outputFrame == globals::state->frameCount ? specularMask : 0u; }
void InvalidateStereo() { if (eye == 0) { invalidate = true; cropDepthCS=nullptr; } }
NRD* RightNRD() { return rightNRD.get(); }
const NRD* RightNRDDebug() { return rightNRD && rightNRD->texNRDViewZ ? rightNRD.get() : nullptr; }
const ScreenSpaceGI* RightGIDebug() { return rightGI && rightGI->texNRDInput ? rightGI.get() : nullptr; }
const ScreenSpaceReflections* RightSSRDebug() { return rightSSR && rightSSR->texNRDSpecInput ? rightSSR.get() : nullptr; }
ID3D11ShaderResourceView* Crop(Input slot, ID3D11ShaderResourceView* source) {
    if (!globals::game::isVR || !source) return source;
    winrt::com_ptr<ID3D11Resource> resource;
    source->GetResource(resource.put());
    auto tex = resource.as<ID3D11Texture2D>();
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{}; source->GetDesc(&view);
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip != 0 || d.SampleDesc.Count != 1 || d.ArraySize != 1)
        throw std::runtime_error("NRD: unsupported packed stereo input texture");
    auto full = FullSize(); auto active = ActiveSize();
    const UINT w = static_cast<UINT>(full.x), h = static_cast<UINT>(full.y);
    const UINT aw = static_cast<UINT>(active.x), ah = static_cast<UINT>(active.y), x = eye * aw;
    if (!w || !h || x + aw > d.Width || ah > d.Height)
        throw std::runtime_error("NRD: packed stereo input extent mismatch");
    auto& c = crops[static_cast<size_t>(slot)];
    if (slot == Input::Depth) {
        if (!c.texture || c.desc.Width != w || c.desc.Height != h) {
            c={};
            D3D11_TEXTURE2D_DESC cd{}; cd.Width=w; cd.Height=h; cd.MipLevels=1; cd.ArraySize=1;
            cd.Format=DXGI_FORMAT_R32_FLOAT; cd.SampleDesc.Count=1; cd.Usage=D3D11_USAGE_DEFAULT;
            cd.BindFlags=D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            auto* device=globals::d3d::device;
            DX::ThrowIfFailed(device->CreateTexture2D(&cd,nullptr,c.texture.put()));
            DX::ThrowIfFailed(device->CreateShaderResourceView(c.texture.get(),nullptr,c.srv.put()));
            DX::ThrowIfFailed(device->CreateUnorderedAccessView(c.texture.get(),nullptr,c.uav.put()));
            c.desc=cd;
        }
        if (!cropDepthCS) cropDepthCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VR\\NRDCropEyeDepth.cs.hlsl",{},"cs_5_0")));
        if (!cropDepthCS) throw std::runtime_error("NRD: stereo depth crop shader failed");
        auto* ctx=globals::d3d::context;
        BindEye();
        ID3D11UnorderedAccessView* uav=c.uav.get();
        ctx->CSSetShaderResources(31,1,&source); ctx->CSSetUnorderedAccessViews(7,1,&uav,nullptr);
        ctx->CSSetShader(cropDepthCS.get(),nullptr,0); ctx->Dispatch((w+7)/8,(h+7)/8,1);
        source=nullptr;uav=nullptr;
        ctx->CSSetShaderResources(31,1,&source); ctx->CSSetUnorderedAccessViews(7,1,&uav,nullptr);
        return c.srv.get();
    }

    if (!c.texture || c.desc.Width != w || c.desc.Height != h || c.desc.Format != d.Format || c.viewFormat != view.Format) {
        c = {};
        d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1;
        d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags=0; d.MiscFlags=0;
        view.Texture2D.MipLevels=1;
        DX::ThrowIfFailed(globals::d3d::device->CreateTexture2D(&d,nullptr,c.texture.put()));
        DX::ThrowIfFailed(globals::d3d::device->CreateShaderResourceView(c.texture.get(),&view,c.srv.put()));
        c.desc=d; c.viewFormat=view.Format;
    }
    const D3D11_BOX box{x,0,0,x+aw,ah,1};
    globals::d3d::context->CopySubresourceRegion(c.texture.get(),0,0,0,0,tex.get(),0,&box);
    return c.srv.get();
}
FrameOutputs Run() {
    FrameOutputs out{}; diffuseMask=specularMask=0; eye=0;
    outputFrame=globals::state->frameCount;
    if (!globals::game::isVR) return out;
    auto& gi=globals::features::screenSpaceGI;
    auto& ssr=globals::features::screenSpaceReflections;
    auto& nrd=globals::features::nrd;
    if (gi.loaded) gi.RefreshE11Overlay();
    if (ssr.loaded) ssr.RefreshE11Overlay();
    if (globals::state->isMapMenuOpen || globals::state->IsMainOrLoadingMenuOpen()) { gi.QueueHistoryReset(); ssr.QueueHistoryReset(); InvalidateStereo(); globals::state->UpdateFeatureData(true); return out; }
    const bool refreshRight = invalidate || gi.recompileFlag || ssr.recompileFlag;
    // Snapshot before eye 0 consumes its atomic flags. History-only changes
    // must reach eye 1, without recompiling NRD guides every disabled frame.
    const bool resetRightGI = gi.queuedResetHistory.load(std::memory_order_acquire);
    const bool resetRightSSR = ssr.queuedHistoryReset.load(std::memory_order_acquire) ||
        ssr.queuedEnabledChange.load(std::memory_order_acquire);
    invalidate=false;
    constexpr uint32_t count=2;
    // Each feature instance owns its own permanent history, including NRD pools.
    for (eye=0;eye<count;++eye) {
        try {
            ScreenSpaceGI* g=&gi; ScreenSpaceReflections* s=&ssr; NRD* n=&nrd;
            if (eye) {
                if (nrd.loaded && !rightNRD) { rightNRD=std::make_unique<NRD>(); rightNRD->loaded=true; rightNRD->settings=nrd.settings; rightNRD->SetupResources(); }
                if (rightNRD) { n=rightNRD.get(); n->loaded=nrd.loaded; n->settings=nrd.settings; if(refreshRight) n->ClearShaderCache(); }
                if (gi.loaded && gi.settings.Enabled && !rightGI) { rightGI=std::make_unique<ScreenSpaceGI>(); rightGI->loaded=true; rightGI->settings=gi.settings; rightGI->userSettings=gi.userSettings; rightGI->SetupResources(); }
                if (rightGI) { g=rightGI.get(); g->loaded=gi.loaded; if (!(g->settings==gi.settings) || refreshRight) { g->settings=gi.settings; g->userSettings=gi.userSettings; g->appliedE11Overlay=gi.appliedE11Overlay; g->recompileFlag=true; g->QueueHistoryReset(); } }
                if (rightGI && resetRightGI) g->QueueHistoryReset();
                if (ssr.loaded && ssr.settings.Enabled && !rightSSR) { rightSSR=std::make_unique<ScreenSpaceReflections>(); rightSSR->loaded=true; rightSSR->settings=ssr.settings; rightSSR->userSettings=ssr.userSettings; rightSSR->SetupResources(); }
                if (rightSSR) { s=rightSSR.get(); s->loaded=ssr.loaded; if (!(s->settings==ssr.settings) || refreshRight) { s->settings=ssr.settings; s->userSettings=ssr.userSettings; s->appliedE11Overlay=ssr.appliedE11Overlay; s->recompileFlag=true; } }
                if (rightSSR && (resetRightSSR || (gi.settings.Enabled && resetRightGI))) s->QueueHistoryReset();
            }
            // Never accidentally dispatch a left-eye feature for an unallocated right eye.
            const bool runGI=g->loaded && g->settings.Enabled && (!eye || rightGI);
            const bool runSSR=s->loaded && s->settings.Enabled && (!eye || rightSSR);
            const bool guides=(runGI && g->settings.EnableREBLUR) || (runSSR && s->WantsReblur());
            if (n->loaded && (!eye || rightNRD)) n->PrepareGuides(guides, true);
            globals::state->UpdateFeatureData(true);
            if (runGI) {
                g->DrawSSGI();
                out.diffuse[eye]=g->GetDiffuseOutputTexture();
                if (out.diffuse[eye]) {
                    diffuseMask|=1u<<eye;
                    globals::state->UpdateFeatureData(true);
                    g->Composite();
                }
            }
            else if (!eye && g->loaded) g->DrawSSGI();
            if (runSSR) { s->DrawSSR(); out.specular[eye]=s->GetOutputTexture(); if(out.specular[eye]) specularMask|=1u<<eye; }
            else if (!eye && s->loaded) s->DrawSSR();
        } catch (const std::exception& e) {
            logger::error("NRD: eye {} dispatch failed: {}",eye,e.what());
            NRDRendering::ClearComputeBindings();
            diffuseMask &= ~(1u << eye); specularMask &= ~(1u << eye);
            out.diffuse[eye]=nullptr; out.specular[eye]=nullptr;
            gi.QueueHistoryReset(); ssr.QueueHistoryReset(); invalidate=true;
        }
    }
    eye=0;
    if (!gi.settings.Enabled || !gi.loaded) rightGI.reset();
    if (!ssr.settings.Enabled || !ssr.loaded) rightSSR.reset();
    if (!nrd.loaded) rightNRD.reset();
    globals::state->UpdateFeatureData(true);
    NRDRendering::UnbindEye();
    return out;
}
}
