#pragma once
#include <d3d11.h>
struct NRD;
namespace NRDRendering
{
	enum class Input : uint32_t { Depth, Normal, Main, Albedo, Masks, Motion, Count };
	uint32_t Eye();
	float2 FullSize();
	float2 ActiveSize();
	void EyeTextureDesc(D3D11_TEXTURE2D_DESC& desc);
	ID3D11ShaderResourceView* Crop(Input slot, ID3D11ShaderResourceView* source);
	void BindEye(bool replaceAmbient = false, float saturation = 1.0f);
	void UnbindEye();
	NRD& Service();
	void LogMemory(const char* phase, uint32_t width, uint32_t height);
	void ClearComputeBindings();
	bool ShouldDeferAllocation();
}
