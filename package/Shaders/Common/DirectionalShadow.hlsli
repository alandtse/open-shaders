#ifndef __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__
#define __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"

namespace DirectionalShadow
{
	// Single owner of the directional-shadow routing decision, so new consumers
	// can't read the stale engine mask under SLF. Under LIGHT_LIMIT_FIX the engine
	// mask is a no-op, so sample LLF cascades directly (fully lit past range);
	// mask-backed VR callers are the exception and keep the engine mask path to
	// avoid receiver banding after the rasterizer safety split.
	// Also owns the PCF noise-rotation so callers skip the sincos boilerplate.
	// Lighting.hlsl deliberately bypasses this helper (feeds the mask into LLF
	// as the past-cascade fallback). Needs LightLimitFix.hlsli first.
#if defined(LIGHT_LIMIT_FIX)
	float SampleLightLimitFixDirectionalShadow(float3 worldPosition, float3 worldPositionWS, uint eyeIndex, float a_screenNoise)
	{
		float2 rotation;
		sincos(Math::TAU * a_screenNoise, rotation.y, rotation.x);
		float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
		return LightLimitFix::GetDirectionalShadow(worldPosition, worldPositionWS, rotationMatrix, eyeIndex);
	}
#endif

	float GetSceneDirectionalShadow(float3 worldPosition, float3 worldPositionWS, uint eyeIndex, float a_screenNoise, float a_engineMaskShadow)
	{
#if defined(LIGHT_LIMIT_FIX)
#	if defined(VR)
		// Keep masked VR callers on the engine-produced mask so they stay
		// aligned with Utility.hlsl's receiver bias.
		return a_engineMaskShadow;
#	else
		return SampleLightLimitFixDirectionalShadow(worldPosition, worldPositionWS, eyeIndex, a_screenNoise);
#	endif
#else
		return a_engineMaskShadow;
#endif
	}

	// Maskless callers (for example particles) still need the direct LLF path
	// because they have no engine shadowmask sample to return in VR.
	float GetSceneDirectionalShadow(float3 worldPosition, float3 worldPositionWS, uint eyeIndex, float a_screenNoise)
	{
#if defined(LIGHT_LIMIT_FIX)
		return SampleLightLimitFixDirectionalShadow(worldPosition, worldPositionWS, eyeIndex, a_screenNoise);
#else
		return 1.0;
#endif
	}
}

#endif  // __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__
