#ifndef __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__
#define __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"

namespace DirectionalShadow
{
	float GetSceneDirectionalShadow(float3 worldPosition, float3 worldPositionWS, uint eyeIndex, float a_screenNoise, float a_engineMaskShadow, out float directionalCoverage, out float nativeContactWeight)
	{
#if defined(LIGHT_LIMIT_FIX)
		float2 rotation;
		sincos(Math::TAU * a_screenNoise, rotation.y, rotation.x);
		float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
		return LightLimitFix::GetDirectionalShadow(worldPosition, worldPositionWS, rotationMatrix, eyeIndex, directionalCoverage, nativeContactWeight);
#else
		directionalCoverage = 1.0;
		nativeContactWeight = 1.0;
		return a_engineMaskShadow;
#endif
	}

	float GetSceneDirectionalShadow(float3 worldPosition, float3 worldPositionWS, uint eyeIndex, float a_screenNoise, float a_engineMaskShadow, out float directionalCoverage)
	{
		float nativeContactWeight;
		return GetSceneDirectionalShadow(worldPosition, worldPositionWS, eyeIndex, a_screenNoise, a_engineMaskShadow, directionalCoverage, nativeContactWeight);
	}

	float GetSceneDirectionalShadow(float3 worldPosition, float3 worldPositionWS, uint eyeIndex, float a_screenNoise, float a_engineMaskShadow)
	{
		float directionalCoverage;
		return GetSceneDirectionalShadow(worldPosition, worldPositionWS, eyeIndex, a_screenNoise, a_engineMaskShadow, directionalCoverage);
	}
}

#endif  // __DIRECTIONAL_SHADOW_DEPENDENCY_HLSL__
