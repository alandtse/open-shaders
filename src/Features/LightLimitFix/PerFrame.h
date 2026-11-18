#pragma once

#include <cstdint>

// CPU mirror of SharedData::LightLimitFixSettings (package/Shaders/Common/SharedData.hlsli),
// split out of LightLimitFix.h so tests can check the layout without the game runtime.
namespace LightLimitFixCB
{
	struct alignas(16) PerFrame
	{
		uint32_t EnableContactShadows;
		uint32_t ContactShadowMaxSteps;
		float ContactShadowMaxDistance;
		float ContactShadowLength;
		float ContactShadowDepthThickness;
		float ContactShadowMinIntensity;
		uint32_t ShadowMapSlots;  // total shadow map texture-array capacity
		// Removing this shifts ClusterSize off its required 16-byte boundary and the GPU reads
		// the cluster config from the wrong offsets. SharedData::LightLimitFixSettings mirrors it.
		uint32_t SunLitHair;  // 1: the forward hair pass reuses the directional shadow the deferred hair pass stored
		// Cluster config (computed)
		uint32_t ClusterSize[4];
		// Debug (last)
		uint32_t EnableLightsVisualisation;
		uint32_t LightsVisualisationMode;
		uint32_t EnableParticleContactShadows;
		uint32_t ClusteredLightCount;
	};
	static_assert(sizeof(PerFrame) % 16 == 0, "PerFrame is not a multiple of 16.");
	// Update SharedData::LightLimitFixSettings in step with any layout change, then this size.
	static_assert(sizeof(PerFrame) == 64,
		"LightLimitFix::PerFrame layout drifted -- update SharedData::LightLimitFixSettings in package/Shaders/Common/SharedData.hlsli to match, then update this assert.");
}
