#ifndef __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_MISTS_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_MISTS_HLSLI__

#include "Common/ValueNoise.hlsli"
#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

namespace ExponentialHeightFog
{
	static const float kMistGridRangeFraction = 0.9f;
	static const float kMistRangeFadeStart = 0.5f;
	static const float kMistWeatherReferenceDistance = 8192.0f;
	static const float kMistMaxReferenceOpticalDepth = 0.75f;
	static const float kMistPocketOpticalDepthScale = 3.0f;
	static const float2 kMistResolutionFade = float2(0.25f, 0.75f);
	static const float2 kMistPocketThresholds = float2(0.35f, 0.7f);
	static const float kMistDetailFrequency = 3.0f;
	static const float kMistDetailAmount = 0.45f;
	static const float kMistWarpAmount = 1.25f;
	static const float kMistDetailOffset = 19.0f;
	static const float3 kMistShapeFrequency = float3(0.4f, 1.0f, 2.0f);
	static const float3 kMistEvolutionDirection = float3(0.17f, -0.23f, 0.31f);

	float3 MistShapeCoordinates(float3 position)
	{
		float2 alongWind = VolumetricFogMistShapeParameters.xy;
		float2 acrossWind = float2(-alongWind.y, alongWind.x);
		return float3(dot(position.xy, alongWind), dot(position.xy, acrossWind), position.z) * kMistShapeFrequency;
	}

	float EvaluateNearbyMistExtinction(uint3 coord, float3 positionWS, uint eyeIndex)
	{
		float extinction = 0.0f;
		[branch] if (SharedData::exponentialHeightFogSettings.mistsEnabled != 0 && SharedData::exponentialHeightFogSettings.mistStrength > 0.0f)
		{
			float range = min(SharedData::exponentialHeightFogSettings.mistRange, GetVolumetricEndDistance() * kMistGridRangeFraction);
			float3 cameraWS = FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
			float3 volumeCenter = FrameBuffer::CameraPosAdjust[0].xyz;
#if defined(VR)
			volumeCenter = (volumeCenter + FrameBuffer::CameraPosAdjust[1].xyz) * 0.5f;
#endif
			float3 absolutePosition = positionWS + cameraWS;
			float distance = length(absolutePosition - volumeCenter);
			[branch] if (distance < range)
			{
				float weatherOpticalDepth = EvaluateFogExtinctionSegment(0.0f, kMistWeatherReferenceDistance, positionWS, cameraWS) * kMistWeatherReferenceDistance;
				float pocketOpticalDepth = clamp(weatherOpticalDepth, 0.0f, kMistMaxReferenceOpticalDepth) *
				                           kMistPocketOpticalDepthScale * SharedData::exponentialHeightFogSettings.mistStrength;
				[branch] if (pocketOpticalDepth > 0.0f)
				{
					float pocketExtinction = pocketOpticalDepth / SharedData::exponentialHeightFogSettings.mistSize;

					uint cornerEye;
					float cornerDepth;
					float3 cellFront = ComputeCellWorldPosition(coord, 0.0f.xxx, cornerEye, cornerDepth);
					float3 cellBack = ComputeCellWorldPosition(coord, 1.0f.xxx, cornerEye, cornerDepth);
					float footprint = length(MistShapeCoordinates(cellBack - cellFront)) / SharedData::exponentialHeightFogSettings.mistSize;
					float resolvedWeight = 1.0f - smoothstep(kMistResolutionFade.x, kMistResolutionFade.y, footprint);
					[branch] if (resolvedWeight > 0.0f)
					{
						float3 noisePosition = MistShapeCoordinates(absolutePosition - float3(VolumetricFogMistDriftOffset, 0.0f)) /
						                       SharedData::exponentialHeightFogSettings.mistSize;
						float3 evolution = VolumetricFogMistShapeParameters.z * kMistEvolutionDirection;
						float3 broadNoise = ValueNoise::Evaluate3(noisePosition + evolution);
						float detailWeight = 1.0f - smoothstep(kMistResolutionFade.x, kMistResolutionFade.y, footprint * kMistDetailFrequency);
						float shape = broadNoise.x;
						[branch] if (detailWeight > 0.0f)
						{
							float detail = ValueNoise::Evaluate3(noisePosition * kMistDetailFrequency + (broadNoise.yzx - 0.5f) * kMistWarpAmount + kMistDetailOffset - evolution).x;
							shape = lerp(shape, detail, kMistDetailAmount * detailWeight);
						}
						float pockets = smoothstep(kMistPocketThresholds.x, kMistPocketThresholds.y, shape);
						float rangeWeight = 1.0f - smoothstep(range * kMistRangeFadeStart, range, distance);
						extinction = pocketExtinction * pockets * rangeWeight * resolvedWeight;
					}
				}
			}
		}
		return extinction;
	}
}

#endif
