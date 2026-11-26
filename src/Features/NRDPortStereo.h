#pragma once
#include <array>
#include <d3d11.h>
struct NRD;
struct ScreenSpaceGI;
struct ScreenSpaceReflections;
namespace NRDPort {
    enum class Input : uint32_t { Depth, Normal, Main, Albedo, Masks, Motion, Count };
    struct FrameOutputs {
        std::array<ID3D11ShaderResourceView*,2> diffuse{};
        std::array<ID3D11ShaderResourceView*,2> specular{};
    };
    uint32_t Eye();
    float2 FullSize();
    float2 ActiveSize();
    void EyeTextureDesc(D3D11_TEXTURE2D_DESC& desc);
    ID3D11ShaderResourceView* Crop(Input slot, ID3D11ShaderResourceView* source);
    void BindEye(bool replaceAmbient = false, float saturation = 1.0f);
    NRD& Service();
    const NRD* RightNRDDebug();
    const ScreenSpaceGI* RightGIDebug();
    const ScreenSpaceReflections* RightSSRDebug();
    uint32_t DiffuseMask();
    uint32_t SpecularMask();
    void InvalidateStereo();
    FrameOutputs Run();
}
