#ifndef __RAIN_CURTAIN_DEPENDENCY_HLSL__
#define __RAIN_CURTAIN_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/ValueNoise.hlsli"

namespace RainCurtain
{
	static const float3 BandDirection = float3(0.8192319, 0.5734624, 0.0);
	static const float BandAspect = 0.28;
	static const float BandHeightScale = 2.0;
	static const float2 BandThreshold = float2(0.3, 0.7);
	static const float BandMinimumDensity = 0.35;
	static const float ClusterScale = 0.6;
	static const float2 ClusterThreshold = float2(0.25, 0.75);
	static const float ClusterMinimumDensity = 0.5;
	static const float FadeWidth = 0.1;
	static const float HashToUnit = 1.0 / 16777216.0;

	/** @brief Stable per-drop random value from the drop's base vertex position. */
	float DropHash(float3 basePosition)
	{
		return float(Random::pcg3d(asuint(basePosition)).x >> 8u) * HashToUnit;
	}

	/** @brief Horizontal distance the sheets have travelled downwind: the ambient wind direction times the accumulated gust travel. */
	float2 Drift()
	{
		float2 ambientWind = SharedData::WindFieldAmbient.xy;
		float ambientLength = length(ambientWind);
		return ambientLength > EPSILON_DIVISION ? ambientWind / ambientLength * SharedData::WindFieldAmbient.w : 0.0;
	}

	/** @brief Per-drop visibility from world-anchored sheets and clusters offset by `drift`; drops whose hash exceeds the local density are culled. Strength 0 returns 1. */
	float Weight(float3 worldPosition, float2 drift, float dropHash, float scale, float strength)
	{
		float3 position = worldPosition - float3(drift, 0.0);
		float3 bandCoordinate = float3(
			dot(position, BandDirection) / (scale * BandAspect),
			dot(position.xy, float2(-BandDirection.y, BandDirection.x)) / scale,
			position.z / (scale * BandHeightScale));
		float band = smoothstep(BandThreshold.x, BandThreshold.y, ValueNoise::Evaluate3(bandCoordinate).x);
		float cluster = smoothstep(ClusterThreshold.x, ClusterThreshold.y, ValueNoise::Evaluate3(position / (scale * ClusterScale)).y);
		float density = lerp(1.0, lerp(BandMinimumDensity, 1.0, band) * lerp(ClusterMinimumDensity, 1.0, cluster), strength);
		return density >= 1.0 ? 1.0 : saturate((density - dropHash) / FadeWidth);
	}
}

#endif
