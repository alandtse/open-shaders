#pragma once
#include <array>
#include <d3d11.h>
#include "Features/NRD/RenderContext.h"
struct NRD;
struct ScreenSpaceGI;
struct ScreenSpaceReflections;
namespace NRDStereo
{
	struct FrameOutputs {
		std::array<ID3D11ShaderResourceView*, 2> diffuse{};
		std::array<ID3D11ShaderResourceView*, 2> specular{};
	};
	uint32_t Eye();
	NRD* RightNRD();
	const NRD* RightNRDDebug();
	const ScreenSpaceGI* RightGIDebug();
	const ScreenSpaceReflections* RightSSRDebug();
	uint32_t DiffuseMask();
	uint32_t SpecularMask();
	void InvalidateStereo();
	ID3D11ShaderResourceView* Crop(NRDRendering::Input slot, ID3D11ShaderResourceView* source);
	FrameOutputs Run();
}
