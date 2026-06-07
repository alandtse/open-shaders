#include "ShadowmapCascadeRasterizerFix.h"

void ShadowmapRasterizerFix::Install()
{
	gRasterStates = reinterpret_cast<RasterStatePtr*>(REL::RelocationID(524748, 411363).address());

	// VR's gRasterStates has 13 depth-bias presets vs 12 on flat; stride accordingly so the
	// per-cascade swap lands in the slots the engine actually binds (fixes VR cascade flicker).
	depthDim = globals::game::isVR ? 13 : 12;

	numCascades = static_cast<uint>(Util::GetGameSettingValue<std::int32_t>("iNumSplits:Display", Settings.at("iNumSplits:Display")));

	// Install the hook LAST, after all static state is set, so a render-thread RenderCascade
	// can't enter thunk() with a null gRasterStates or an unset VR stride. The hooked function
	// is called once per cascade to begin the updating and rendering process.
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6, 0xF6));
}

void ShadowmapRasterizerFix::BSShadowDirectionalLight_RenderShadowmaps_RenderCascade::thunk(RE::BSShadowDirectionalLight* light, void* arg1, void* arg2, uint32_t flags)
{
	static uint cascade = 0;

	const auto bytes = static_cast<std::size_t>(StateCount()) * sizeof(RasterStatePtr);

	static bool initialized = false;
	if (!initialized) {
		//Backup
		if (cascade == 0) {
			std::memcpy(backupGameRasterStates, gRasterStates, bytes);
			numCascades = std::max(1u, std::min(numCascades, maxCascades));
		}

		//Clone from the pristine engine table (we overwrite gRasterStates per cascade below)
		CloneRasterStates(backupGameRasterStates, cascade);

		initialized = cascade == numCascades - 1;
	}

	//Emplace
	std::memcpy(gRasterStates, shadowmapRasterStates[cascade], bytes);

	func(light, arg1, arg2, flags);

	//Restore
	if (cascade == numCascades - 1)
		std::memcpy(gRasterStates, backupGameRasterStates, bytes);

	cascade = ++cascade < numCascades ? cascade : 0;
}

void ShadowmapRasterizerFix::GetUpdatedRasterDesc(D3D11_RASTERIZER_DESC& outputDesc, ShadowMapRasterizerDescriptor shadowmapDesc)
{
	outputDesc.DepthBias = shadowmapDesc.rasterDepthBias;
	outputDesc.DepthBiasClamp = shadowmapDesc.rasterDepthBiasClamp;
	outputDesc.SlopeScaledDepthBias = shadowmapDesc.rasterSlopeScaleBias;
}

// Since state objects are shared globally across the pipeline we make duplicate arrays that cover the same range of states the game does
void ShadowmapRasterizerFix::CloneRasterStates(RasterStatePtr* inputArray, int cascade)
{
	for (int fill = 0; fill < kFill; fill++) {
		for (int cull = 0; cull < kCull; cull++) {
			for (int depth = 0; depth < depthDim; depth++) {
				for (int scissor = 0; scissor < kScissor; scissor++) {
					const int i = StateIndex(fill, cull, depth, scissor);
					if (auto* gRasterizer = inputArray[i]) {
						D3D11_RASTERIZER_DESC desc{};
						gRasterizer->GetDesc(&desc);

						GetUpdatedRasterDesc(desc, cascadeDescriptors[cascade]);

						// Degrade instead of crashing: on failure keep the engine's own state for this slot
						// (loses only our added bias, not its cull/fill/scissor) rather than D3D defaults.
						auto*& clonedRaster = shadowmapRasterStates[cascade][i];
						if (const auto hr = globals::d3d::device->CreateRasterizerState(&desc, &clonedRaster); FAILED(hr)) {
							logger::warn("ShadowmapRasterizerFix: failed to clone cascade {} rasterizer state (hr=0x{:08X}); keeping engine state", cascade, static_cast<std::uint32_t>(hr));
							clonedRaster = gRasterizer;
						}
					}
				}
			}
		}
	}
}