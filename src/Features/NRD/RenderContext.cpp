#include "RenderContext.h"
#include "Features/NRD.h"
#include "Features/VR/NRDStereo.h"
#include "Globals.h"
#include "State.h"
#include "ShaderCache.h"
#include "Util.h"
#include "Buffer.h"
#include <dxgi1_4.h>
#include <psapi.h>

namespace NRDRendering
{
namespace { std::unique_ptr<ConstantBuffer> eyeCB; }
uint32_t Eye() { return globals::game::isVR ? NRDStereo::Eye() : 0u; }
NRD& Service() {
    if (globals::game::isVR && Eye()) {
        if (auto* right = NRDStereo::RightNRD()) return *right;
    }
    return globals::features::nrd;
}
void EyeTextureDesc(D3D11_TEXTURE2D_DESC& d) { if (globals::game::isVR) d.Width /= 2; }
float2 FullSize() {
    auto tex = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
    D3D11_TEXTURE2D_DESC d{};
    if (tex) Util::AsReal(tex)->GetDesc(&d);
    EyeTextureDesc(d);
    return {float(d.Width),float(d.Height)};
}
float2 ActiveSize() {
    auto full = FullSize();
    auto packed = full;
    if (globals::game::isVR) packed.x *= 2;
    auto active = Util::ConvertToDynamic(packed, true);
    if (globals::game::isVR) active.x *= 0.5f;
    if (full.x < 1.0f || full.y < 1.0f) return {0.f, 0.f};
    return {std::clamp(std::floor(active.x),1.f,std::max(full.x,1.f)),std::clamp(std::floor(active.y),1.f,std::max(full.y,1.f))};
}
ID3D11ShaderResourceView* Crop(Input slot, ID3D11ShaderResourceView* source) {
    return globals::game::isVR ? NRDStereo::Crop(slot, source) : source;
}
void BindEye(bool replaceAmbient, float saturation) {
    if (!eyeCB) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth=32; d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        eyeCB=std::make_unique<ConstantBuffer>(d,"NRD eye-local constants");
    }
    const auto active=ActiveSize();
    struct alignas(16) Constants { uint32_t eye, offset, width, height, replace; float saturation; float pad[2]; };
    Constants c{Eye(),Eye()*static_cast<uint32_t>(active.x),static_cast<uint32_t>(active.x),static_cast<uint32_t>(active.y),replaceAmbient?1u:0u,saturation,{}};
    eyeCB->Update(c); auto* cb=eyeCB->CB(); globals::d3d::context->CSSetConstantBuffers(3,1,&cb);
}
void UnbindEye() {
    ID3D11Buffer* nullCB = nullptr;
    globals::d3d::context->CSSetConstantBuffers(3, 1, &nullCB);
}
void ClearComputeBindings() {
    auto* ctx = globals::d3d::context;
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    ctx->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
    ctx->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    UnbindEye();
}
bool ShouldDeferAllocation() {
    return globals::state->IsMainOrLoadingMenuOpen() || (globals::shaderCache && globals::shaderCache->IsCompiling());
}
void LogMemory(const char* phase, uint32_t width, uint32_t height) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        logger::info("[NRD] {} {}x{}: physical available={} MiB, process commit headroom={} MiB (limit={} MiB)",
            phase, width, height, mem.ullAvailPhys / MiB, mem.ullAvailPageFile / MiB, mem.ullTotalPageFile / MiB);
    PERFORMANCE_INFORMATION perf{};
    perf.cb = sizeof(perf);
    if (K32GetPerformanceInfo(&perf, sizeof(perf)))
        logger::info("[NRD] {}: system commit={} MiB / limit={} MiB", phase, uint64_t(perf.CommitTotal) * perf.PageSize / MiB, uint64_t(perf.CommitLimit) * perf.PageSize / MiB);
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (globals::d3d::device && SUCCEEDED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) {
        winrt::com_ptr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.put()))) {
            if (auto adapter3 = adapter.try_as<IDXGIAdapter3>()) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
                    logger::info("[NRD] {}: local GPU memory usage={} MiB / budget={} MiB", phase, memory.CurrentUsage / MiB, memory.Budget / MiB);
            }
        }
    }
}
}
