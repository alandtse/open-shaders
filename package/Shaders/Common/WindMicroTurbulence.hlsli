#ifndef __WIND_MICRO_TURBULENCE_HLSLI__
#define __WIND_MICRO_TURBULENCE_HLSLI__

#include "Common/Math.hlsli"
#include "Common/WindFieldTypes.hlsli"

namespace WindField
{
	namespace Detail
	{
		float GradientNoise(float2 position, uint seed, WindTuning tuning, uint latticeMask);

		static const float MicroSlowSpatialScale = 768.0f;
		static const float MicroDetailSpatialScale = 128.0f;
		static const float MicroMaximumStrength = 4.0f;
		static const float MicroCrosswindSlope = 0.06992681f;
		static const float MicroCrosswindFraction = 0.07f;
		static const float MicroCalmSpeed = 0.02f;
		static const float MicroMaximumNegativeResponse = 0.8f;
		static const float MicroBandWindShare = 0.5f;
		static const uint MicroNoiseMask = 255u;
		static const float MicroNoisePeriod = 256.0f;
		static const uint MicroSlowSeed = (1u << 31) | 0x2511E9B3u;
		static const uint MicroDetailSeed = 0x63D83595u;
		static const uint MicroCrosswindSeed = (1u << 31) | 0x35297A4Du;

		float2 SampleFineBand(float2 worldPosition, FineState state, float scale,
			float spatialScale, float travelScale, uint seed, WindTuning tuning)
		{
			spatialScale = max(spatialScale, EPSILON_WIND_GEOMETRY) / scale;
			travelScale = max(travelScale, 0.0f) * spatialScale;
			float2 coarse = state.travel.xy * travelScale;
			coarse -= floor(coarse / MicroNoisePeriod) * MicroNoisePeriod;
			float2 coordinate = worldPosition * spatialScale - coarse - state.travel.zw * travelScale;
			return clamp(2.0f * float2(
									GradientNoise(coordinate, seed, tuning, MicroNoiseMask),
									GradientNoise(coordinate + float2(17.0f, 31.0f), seed ^ MicroCrosswindSeed, tuning, MicroNoiseMask)),
				-1.0f, 1.0f);
		}

		float3 ResolveFineBand(float2 noise, float3 direction, float speed, float amplitude)
		{
			float budget = speed * MicroBandWindShare;
			float along = amplitude * noise.x;
			float across = amplitude * MicroCrosswindFraction * noise.y;
			float responseScale = along < -MicroMaximumNegativeResponse * budget ?
			                          MicroMaximumNegativeResponse * budget / -along :
			                          1.0f;
			float crosswindDivisor = abs(across) - MicroCrosswindSlope * along;
			if (crosswindDivisor > 0.0f)
				responseScale = min(responseScale, MicroCrosswindSlope * budget / crosswindDivisor);
			float3 crosswind = float3(-direction.y, direction.x, 0.0f);
			return (direction * along + crosswind * across) * responseScale;
		}
	}

	float3 ResolveFineVelocity(float2 noise, float3 baseVelocity, float3 gustVelocity, WindTuning tuning)
	{
		float strength = clamp(tuning.microTurbulenceStrength, 0.0f, Detail::MicroMaximumStrength);
		float baseSpeed = length(baseVelocity.xy);
		float3 ambientVelocity = baseVelocity + gustVelocity;
		float speed = length(ambientVelocity.xy);
		if (strength <= 0.0f || speed <= EPSILON_WIND_RESPONSE)
			return 0.0f.xxx;
		float3 direction = float3(ambientVelocity.xy / speed, 0.0f);
		float3 baseDirection = baseSpeed > EPSILON_WIND_RESPONSE ? float3(baseVelocity.xy / baseSpeed, 0.0f) : direction;
		float positiveGust = max(dot(gustVelocity.xy, baseDirection.xy), 0.0f);
		float amplitude = strength * speed / (speed + Detail::MicroCalmSpeed) *
		                  (max(tuning.fineBaselineAmplitude, 0.0f) +
							  baseSpeed * max(tuning.fineWindCoupling, 0.0f) +
							  positiveGust * max(tuning.fineGustCoupling, 0.0f));
		return Detail::ResolveFineBand(noise, direction, speed, amplitude);
	}

	FineSample SampleFineVelocity(float3 worldPosition, float3 baseVelocity, float3 gustVelocity,
		FineState state, WindTuning tuning, float spatialScale, float travelScale)
	{
		FineSample sample;
		sample.slowVelocity = sample.detailVelocity = 0.0f.xxx;
		if (tuning.microTurbulenceStrength <= 0.0f || length((baseVelocity + gustVelocity).xy) <= EPSILON_WIND_RESPONSE)
			return sample;
		float2 slow = Detail::SampleFineBand(worldPosition.xy, state, Detail::MicroSlowSpatialScale,
			spatialScale, travelScale, tuning.broadGustSeed ^ Detail::MicroSlowSeed, tuning);
		float2 detail = Detail::SampleFineBand(worldPosition.xy, state, Detail::MicroDetailSpatialScale,
			spatialScale, travelScale, tuning.turbulentGustSeed ^ Detail::MicroDetailSeed, tuning);
		sample.slowVelocity = ResolveFineVelocity(slow, baseVelocity, gustVelocity, tuning);
		sample.detailVelocity = ResolveFineVelocity(detail, baseVelocity, gustVelocity, tuning);
		return sample;
	}
}

#endif
