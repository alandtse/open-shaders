#include "ShadowmapCascadeRasterizerFix.h"

#include "Globals.h"

#include <algorithm>

namespace
{
	template <class Fn>
	void ForEachRasterStateSlot(Fn&& fn)
	{
		for (int fill = 0; fill < 2; fill++) {
			for (int cull = 0; cull < 3; cull++) {
				for (int depth = 0; depth < 12; depth++) {
					for (int scissor = 0; scissor < 2; scissor++) {
						fn(fill, cull, depth, scissor);
					}
				}
			}
		}
	}
}

void ShadowmapRasterizerFix::Install()
{
	// This function is called once per cascade to begin the updating and rendering process.
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6, 0xF6));

	gRasterStates = reinterpret_cast<RasterStateArray*>(REL::RelocationID(524748, 411363).address());

	auto configuredCascades = Util::GetGameSettingValue<std::int32_t>("iNumSplits:Display", Settings.at("iNumSplits:Display"));
	numCascades = static_cast<std::uint32_t>(std::clamp(configuredCascades, 1, static_cast<std::int32_t>(maxCascades)));
	currentCascade = 0;
	ReleaseClonedRasterStates();
	initialized = false;
}

void ShadowmapRasterizerFix::BSShadowDirectionalLight_RenderShadowmaps_RenderCascade::thunk(RE::BSShadowDirectionalLight* light, void* arg1, void* arg2, std::uint32_t flags)
{
	// VR bypasses the flat global rasterizer-table swap entirely: swapping the shared global table
	// per cascade is not stereo-safe and produces directional-shadow flicker. The banding fix routes
	// VR directional shadows through the engine mask in the shaders instead.
	if (REL::Module::IsVR() || !gRasterStates) {
		func(light, arg1, arg2, flags);
		return;
	}

	const auto cascade = currentCascade % numCascades;
	if (!initialized) {
		if (cascade == 0) {
			std::memcpy(backupGameRasterStates, *gRasterStates, sizeof(RasterStateArray));
			numCascades = std::min(numCascades, maxCascades);
		}

		CloneRasterStates(*gRasterStates, cascade, flatCascadeDescriptors);

		initialized = cascade == numCascades - 1;
	}

	std::memcpy(*gRasterStates, shadowmapRasterStates[cascade], sizeof(RasterStateArray));

	func(light, arg1, arg2, flags);

	if (cascade == numCascades - 1)
		std::memcpy(*gRasterStates, backupGameRasterStates, sizeof(RasterStateArray));

	currentCascade = (cascade + 1) % numCascades;
}

void ShadowmapRasterizerFix::GetUpdatedRasterDesc(D3D11_RASTERIZER_DESC& outputDesc, ShadowMapRasterizerDescriptor shadowmapDesc)
{
	outputDesc.DepthBias = shadowmapDesc.rasterDepthBias;
	outputDesc.DepthBiasClamp = shadowmapDesc.rasterDepthBiasClamp;
	outputDesc.SlopeScaledDepthBias = shadowmapDesc.rasterSlopeScaleBias;
}

// Since state objects are shared globally across the pipeline we make duplicate arrays that cover the same range of states the game does
void ShadowmapRasterizerFix::CloneRasterStates(const RasterStateArray& inputArray, std::uint32_t cascade, const std::array<ShadowMapRasterizerDescriptor, maxCascades>& descriptors)
{
	ForEachRasterStateSlot([&](int fill, int cull, int depth, int scissor) {
		auto*& clonedRaster = shadowmapRasterStates[cascade][fill][cull][depth][scissor];
		if (clonedRaster) {
			clonedRaster->Release();
			clonedRaster = nullptr;
		}

		if (auto* gRasterizer = inputArray[fill][cull][depth][scissor]) {
			D3D11_RASTERIZER_DESC desc{};
			gRasterizer->GetDesc(&desc);

			GetUpdatedRasterDesc(desc, descriptors[cascade]);

			if (const auto hr = globals::d3d::device->CreateRasterizerState(&desc, &clonedRaster); FAILED(hr)) {
				logger::warn(
					"ShadowmapRasterizerFix: failed to clone rasterizer state for cascade {} (hr=0x{:08X}); using original state",
					cascade,
					static_cast<std::uint32_t>(hr));
				clonedRaster = nullptr;
			}
		}
	});
}

void ShadowmapRasterizerFix::ReleaseClonedRasterStates()
{
	ForEachRasterStateSlot([&](int fill, int cull, int depth, int scissor) {
		for (std::uint32_t cascade = 0; cascade < maxCascades; cascade++) {
			auto*& clonedRaster = shadowmapRasterStates[cascade][fill][cull][depth][scissor];
			if (clonedRaster) {
				clonedRaster->Release();
				clonedRaster = nullptr;
			}
		}
	});
}
