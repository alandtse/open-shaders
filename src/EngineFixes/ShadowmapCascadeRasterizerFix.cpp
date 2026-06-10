#include "ShadowmapCascadeRasterizerFix.h"

#include "Utils/D3D.h"

void ShadowmapRasterizerFix::Install()
{
	gRasterStates = reinterpret_cast<RasterStatePtr*>(REL::RelocationID(524748, 411363).address());

	// VR's gRasterStates has 13 depth-bias presets vs 12 on flat; stride accordingly so the
	// per-cascade swap lands in the slots the engine actually binds (fixes VR cascade flicker).
	depthDim = globals::game::isVR ? 13 : 12;

	// Install the hook LAST, after all static state is set, so a render-thread RenderCascade
	// can't enter thunk() with a null gRasterStates or an unset VR stride. The hooked function
	// is called once per cascade to begin the updating and rendering process.
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6, 0xF6));
}

void ShadowmapRasterizerFix::BSShadowDirectionalLight_RenderShadowmaps_RenderCascade::thunk(RE::BSShadowDirectionalLight* light, void* a_descriptor, void* arg2, uint32_t flags)
{
	// The engine writes the cascade index into the shadowmap descriptor right before this call;
	// read it rather than counting calls so we can never desync from the engine's own loop (its
	// cascade count is not capped at 3 like ours).
	const auto index = REL::RelocateMember<const std::uint32_t>(a_descriptor, 0x58, 0x70);
	const uint cascade = std::min(index, maxCascades - 1);

	const auto bytes = static_cast<std::size_t>(StateCount()) * sizeof(RasterStatePtr);

	static bool backedUp = false;
	if (!backedUp) {
		std::memcpy(backupGameRasterStates, gRasterStates, bytes);
		backedUp = true;
	}

	static bool cloned[maxCascades] = {};
	if (!cloned[cascade]) {
		// Clone from the pristine engine table (we overwrite gRasterStates below)
		CloneRasterStates(backupGameRasterStates, cascade);
		cloned[cascade] = true;
	}

	// Emplace, and force a rebind: the engine issues RSSetState only when a raster dirty flag
	// is set and caches by table index, so swapping table contents alone leaves the previous
	// cascade's state bound until unrelated state traffic happens to dirty it (= flicker).
	std::memcpy(gRasterStates, shadowmapRasterStates[cascade], bytes);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_DEPTH_BIAS);

	func(light, a_descriptor, arg2, flags);

	// Restore unconditionally so the biased table can never leak past this cascade.
	std::memcpy(gRasterStates, backupGameRasterStates, bytes);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_DEPTH_BIAS);
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
							clonedRaster = gRasterizer;  // engine's own state; leave its name as-is
						} else {
							Util::SetResourceName(clonedRaster, "ShadowmapCascadeRasterizerFix::CascadeBias[%d][%d]", cascade, i);
						}
					}
				}
			}
		}
	}
}