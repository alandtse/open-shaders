#ifndef __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__
#define __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"

namespace DirectionalShadow
{
	// Single owner of the directional-shadow routing decision, so new consumers
	// can't read the stale engine mask under SLF. VR keeps the engine mask path
	// because the LLF two-cascade directional sampler can introduce receiver
	// banding after the rasterizer safety split; non-VR still samples LLF cascades
	// directly. Also owns the PCF noise-rotation so callers skip the sincos
	// boilerplate. Lighting.hlsl deliberately bypasses this helper and feeds the
	// mask into LLF directly. Needs LightLimitFix.hlsli first.
	float GetSceneDirectionalShadow(float3 worldPosition, float3 worldPositionWS, uint eyeIndex, float a_screenNoise, float a_engineMaskShadow)
	{
#if defined(LIGHT_LIMIT_FIX)
#	if defined(VR)
		// Keep every VR caller on the engine-produced mask so they stay aligned
		// with Utility.hlsl's receiver bias and never fall back to LLF's direct sampler.
		return a_engineMaskShadow;
#	else
		float2 rotation;
		sincos(Math::TAU * a_screenNoise, rotation.y, rotation.x);
		float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
		return LightLimitFix::GetDirectionalShadow(worldPosition, worldPositionWS, rotationMatrix, eyeIndex);
#	endif
#else
		return a_engineMaskShadow;
#endif
	}
}

#endif  // __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__
