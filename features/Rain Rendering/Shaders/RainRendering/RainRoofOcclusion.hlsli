#ifndef __RAIN_ROOF_OCCLUSION_HLSL__
#define __RAIN_ROOF_OCCLUSION_HLSL__

#define SKYLIGHTING_PROBE_REGISTER t38
#include "Skylighting/Skylighting.hlsli"

Texture3D<float> RainCanopyClassification : register(t41);
Texture3D<uint> RainCanopyAccumulation : register(t42);

namespace RainRoofOcclusion
{
	struct Cover
	{
		float RoofVisibility;
		float CanopyAmount;
	};

	float SampleCanopy(float3 positionMS)
	{
		float3 adjustedPosition = positionMS - SharedData::skylightingSettings.PosOffset.xyz;
		float3 uvw = adjustedPosition / Skylighting::ARRAY_SIZE + 0.5f;
		if (any(uvw < 0.0f) || any(uvw > 1.0f))
			return 0.0f;

		float3 cellCoordinate = uvw * Skylighting::ARRAY_DIM;
		int3 firstCell = int3(floor(cellCoordinate - 0.5f));
		float3 interpolation = cellCoordinate - 0.5f - firstCell;
		float canopy = 0.0f;
		float confidence = 0.0f;
		float weightSum = 0.0f;
		[unroll] for (int x = 0; x < 2; ++x)
			[unroll] for (int y = 0; y < 2; ++y)
				[unroll] for (int z = 0; z < 2; ++z)
		{
			int3 offset = int3(x, y, z);
			int3 cell = firstCell + offset;
			if (any(cell < 0) || any(uint3(cell) >= Skylighting::ARRAY_DIM))
				continue;
			float3 weights = 1.0f - abs(float3(offset) - interpolation);
			float weight = weights.x * weights.y * weights.z;
			uint3 textureCell = (uint3(cell) + SharedData::skylightingSettings.ArrayOrigin.xyz) % Skylighting::ARRAY_DIM;
			canopy += RainCanopyClassification[textureCell] * weight;
			confidence += saturate(float(RainCanopyAccumulation[textureCell]) / 15.0f) * weight;
			weightSum += weight;
		}
		float inverseWeight = rcp(max(weightSum, EPSILON_WEIGHT_SUM));
		return canopy * inverseWeight * confidence * inverseWeight * Skylighting::GetFadeOutFactor(positionMS);
	}

	/** Preserves Skylighting's roof result and relaxes it only for paired tree-only samples. */
	Cover Sample(float3 worldPosition, float fadeStart, float fadeEnd)
	{
		float3 positionMS = worldPosition - FrameBuffer::CameraPosAdjust[0].xyz;
		sh2 skyVisibility = Skylighting::SampleNoBiasIncludingInteriors(positionMS);
		float allGeometryVisibility = Skylighting::CosineLobeVisibility(
			skyVisibility, float3(0.0f, 0.0f, 1.0f), Skylighting::GetFadeOutFactor(positionMS));
		float canopyAmount = Canopy.x > 0.5f ? SampleCanopy(positionMS) : 0.0f;
		float correctedVisibility = saturate(allGeometryVisibility + canopyAmount);
		Cover cover;
		cover.RoofVisibility = smoothstep(
			min(fadeStart, fadeEnd), max(fadeStart, fadeEnd), correctedVisibility);
		cover.CanopyAmount = canopyAmount;
		return cover;
	}
}

#endif  // __RAIN_ROOF_OCCLUSION_HLSL__
