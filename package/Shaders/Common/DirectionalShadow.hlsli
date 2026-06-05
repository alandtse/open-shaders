#ifndef __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__
#define __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"

namespace DirectionalShadow
{
	// Single owner of the directional-shadow routing decision, so new consumers
	// can't read the stale engine mask under SLF. Under LIGHT_LIMIT_FIX the engine
	// mask is a no-op, so sample LLF cascades directly (fully lit past range);
	// VR is the exception and keeps the engine mask path to avoid receiver
	// banding after the rasterizer safety split.
	// Also owns the PCF noise-rotation so callers skip the sincos boilerplate.
	// Lighting.hlsl deliberately bypasses this helper (feeds the mask into LLF
	// as the past-cascade fallback). Needs LightLimitFix.hlsli first.
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
