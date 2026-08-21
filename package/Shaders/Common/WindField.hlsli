#ifndef __WIND_FIELD_DEPENDENCY_HLSL__
#define __WIND_FIELD_DEPENDENCY_HLSL__

namespace WindField
{
	struct WindTuning
	{
		// Supplied by the canonical C++ WindTuning; the GPU implementation defines no defaults.
		float gustScale;
		float frontAspectRatio;
		float gustAdvectionBaseSpeed;
		float gustAdvectionMultiplier;
		float detailScaleRatio;
		float detailCrosswindScaleRatio;
		float turbulenceStrength;
		float turbulenceSkew;
		float contrastLow;
		float contrastHigh;
		float gustAmplitude;
		uint broadGustSeed;
		uint turbulentGustSeed;
		uint gradientSeedMix;
		uint pcgMultiplier;
		uint pcgIncrement;
	};

	struct WindSample
	{
		float3 velocity;
		float ambientGust;
	};

	namespace Detail
	{
		static const float MinimumDivisor = 1e-4f;

		uint2 Pcg2D(uint2 value, WindTuning tuning)
		{
			value = value * tuning.pcgMultiplier + tuning.pcgIncrement;
			value.x += value.y * tuning.pcgMultiplier;
			value.y += value.x * tuning.pcgMultiplier;
			value ^= value >> 16u;
			value.x += value.y * tuning.pcgMultiplier;
			value.y += value.x * tuning.pcgMultiplier;
			return value ^ (value >> 16u);
		}

		float2 GetGradient(uint2 latticePosition, uint seed, WindTuning tuning)
		{
			uint2 hash = Pcg2D(latticePosition ^ uint2(seed, seed ^ tuning.gradientSeedMix), tuning);
			float2 gradient = float2(hash >> 8u) * (2.0f / 16777215.0f) - 1.0f;
			return gradient * rsqrt(max(dot(gradient, gradient), MinimumDivisor));
		}

		float GradientNoise(float2 position, uint seed, WindTuning tuning)
		{
			float2 latticePosition = floor(position);
			float2 offset = position - latticePosition;
			uint2 lattice = asuint(int2(latticePosition));

			float value00 = dot(GetGradient(lattice, seed, tuning), offset);
			float value10 = dot(
				GetGradient(lattice + uint2(1u, 0u), seed, tuning), offset - float2(1.0f, 0.0f));
			float value01 = dot(
				GetGradient(lattice + uint2(0u, 1u), seed, tuning), offset - float2(0.0f, 1.0f));
			float value11 = dot(
				GetGradient(lattice + uint2(1u, 1u), seed, tuning), offset - float2(1.0f, 1.0f));

			float2 fade = offset * offset * offset * (offset * (offset * 6.0f - 15.0f) + 10.0f);
			return lerp(lerp(value00, value10, fade.x), lerp(value01, value11, fade.x), fade.y) * 0.70710678f;
		}

		float Smoothstep(float minimum, float maximum, float value)
		{
			float range = max(maximum - minimum, MinimumDivisor);
			float amount = saturate((value - minimum) / range);
			return amount * amount * (3.0f - 2.0f * amount);
		}

		float3 NormalizeDirection(float3 direction)
		{
			float lengthSquared = dot(direction, direction);
			return lengthSquared > 1e-6f ? direction / sqrt(lengthSquared) : 0.0f;
		}
	}

	/** @brief GPU equivalent of the canonical CPU SampleAmbientGust implementation. */
	float SampleAmbientGust(
		float3 worldPosition, float gustTravelDistance, float3 windDirection, WindTuning tuning)
	{
		float horizontalLengthSquared = dot(windDirection.xy, windDirection.xy);
		float horizontalLength = sqrt(horizontalLengthSquared);
		float2 direction = horizontalLengthSquared > 1e-6f ?
		                       windDirection.xy / horizontalLength :
		                       float2(1.0f, 0.0f);
		float2 crosswindDirection = float2(-direction.y, direction.x);
		float alongWind = dot(worldPosition.xy, direction);
		float acrossWind = dot(worldPosition.xy, crosswindDirection);
		gustTravelDistance = max(gustTravelDistance, 0.0f);
		float gustScale = max(abs(tuning.gustScale), Detail::MinimumDivisor);
		float frontAspectRatio = max(abs(tuning.frontAspectRatio), Detail::MinimumDivisor);
		float2 frontCoordinate = float2(
			(alongWind - gustTravelDistance) / gustScale,
			acrossWind / (gustScale * frontAspectRatio));

		float broadGust = Detail::GradientNoise(frontCoordinate, tuning.broadGustSeed, tuning);
		float detailScaleRatio = max(abs(tuning.detailScaleRatio), Detail::MinimumDivisor);
		float detailCrosswindScaleRatio = max(abs(tuning.detailCrosswindScaleRatio), Detail::MinimumDivisor);
		float2 detailCoordinate = float2(
			frontCoordinate.x / detailScaleRatio + frontCoordinate.y * tuning.turbulenceSkew,
			frontCoordinate.y / detailCrosswindScaleRatio);
		float turbulentGust = Detail::GradientNoise(detailCoordinate, tuning.turbulentGustSeed, tuning);

		float turbulenceStrength = max(tuning.turbulenceStrength, 0.0f);
		float gustNoise = (broadGust + turbulentGust * turbulenceStrength) / (1.0f + turbulenceStrength);
		float normalizedGust = saturate(gustNoise * 0.5f + 0.5f);
		float contrastLow = min(tuning.contrastLow, tuning.contrastHigh);
		float contrastHigh = max(tuning.contrastLow, tuning.contrastHigh);
		return Detail::Smoothstep(contrastLow, contrastHigh, normalizedGust);
	}

	/** @brief GPU equivalent of the canonical CPU SampleWind implementation. */
	WindSample SampleWind(
		float3 worldPosition, float gustTravelDistance, float3 windDirection, float windSpeed, WindTuning tuning)
	{
		WindSample sample;
		sample.ambientGust = SampleAmbientGust(worldPosition, gustTravelDistance, windDirection, tuning);
		float gustDeviation = sample.ambientGust * 2.0f - 1.0f;
		float gustMultiplier = max(1.0f + gustDeviation * max(tuning.gustAmplitude, 0.0f), 0.0f);
		sample.velocity =
			Detail::NormalizeDirection(windDirection) * (max(windSpeed, 0.0f) * gustMultiplier);
		return sample;
	}

	/** @brief GPU equivalent of the canonical CPU SampleAmbientWind implementation. */
	float3 SampleAmbientWind(
		float3 worldPosition, float gustTravelDistance, float3 windDirection, float windSpeed, WindTuning tuning)
	{
		WindSample sample = SampleWind(worldPosition, gustTravelDistance, windDirection, windSpeed, tuning);
		return sample.velocity;
	}
}

#endif  // __WIND_FIELD_DEPENDENCY_HLSL__
