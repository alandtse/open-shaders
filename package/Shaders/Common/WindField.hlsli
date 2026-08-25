#ifndef __WIND_FIELD_DEPENDENCY_HLSL__
#define __WIND_FIELD_DEPENDENCY_HLSL__

namespace WindField
{
	static const uint AmbientGustCapacity = 8u;

	struct AmbientGust
	{
		float2 position;
		float2 direction;
		float speed;
		float length;
		float width;
		float strength;
		float age;
		float lifetime;
		uint seed;
		uint padding;
	};

	struct WindTuning
	{
		// Supplied by the canonical C++ WindTuning; the GPU implementation defines no defaults.
		float gustAmplitude;
		float gustEdgeSoftness;
		float gustNoiseAmount;
		float gustNoiseScale;
		float detailScaleRatio;
		float detailCrosswindScaleRatio;
		float turbulenceStrength;
		float turbulenceSkew;
		float contrastLow;
		float contrastHigh;
		uint broadGustSeed;
		uint turbulentGustSeed;
		uint gradientSeedMix;
		uint pcgMultiplier;
		uint pcgIncrement;
		uint maxActiveGusts;
		float spawnIntervalMin;
		float spawnIntervalMax;
		float gustLengthMin;
		float gustLengthMax;
		float gustWidthMin;
		float gustWidthMax;
		float gustSpeedMin;
		float gustSpeedMax;
		float gustStrengthMin;
		float gustStrengthMax;
		float gustLifetimeMin;
		float gustLifetimeMax;
		float gustAdvectionMultiplier;
		float gustSpawnDistance;
		float2 padding;
	};

	struct WindSample
	{
		float3 velocity;
		float ambientGust;
		float transientImpulse;
	};

	namespace Detail
	{
		static const float MinimumDivisor = 1e-4f;

		struct AmbientBandSample
		{
			float envelope;
			float2 noiseOffset;
		};

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

		float SampleAmbientGustEnvelope(float2 worldPosition, AmbientGust gust, WindTuning tuning)
		{
			float2 delta = worldPosition - gust.position;
			float2 side = float2(-gust.direction.y, gust.direction.x);
			float x = dot(delta, gust.direction) / max(abs(gust.length), MinimumDivisor);
			float y = dot(delta, side) / max(abs(gust.width), MinimumDivisor);
			float edge = saturate(
				(1.0f - (x * x + y * y)) / max(tuning.gustEdgeSoftness, MinimumDivisor));
			return edge * edge * (3.0f - 2.0f * edge);
		}

		AmbientBandSample SampleAmbientBands(float2 worldPosition,
			AmbientGust gusts[AmbientGustCapacity], uint activeGustCount, WindTuning tuning)
		{
			AmbientBandSample sample = (AmbientBandSample)0;
			activeGustCount = min(activeGustCount, AmbientGustCapacity);
			[unroll] for (uint index = 0u; index < AmbientGustCapacity; ++index)
			{
				if (index < activeGustCount) {
					float contribution = SampleAmbientGustEnvelope(worldPosition, gusts[index], tuning) *
					                     max(gusts[index].strength, 0.0f);
					sample.envelope += contribution;
					float2 seedOffset =
						float2(gusts[index].seed & 0xFFFFu, gusts[index].seed >> 16u) * (64.0f / 65535.0f);
					sample.noiseOffset += seedOffset * contribution;
				}
			}
			if (sample.envelope > MinimumDivisor)
				sample.noiseOffset /= sample.envelope;
			sample.envelope = saturate(sample.envelope);
			return sample;
		}
	}

	/** @brief Samples the soft, bounded sum of active world-space gust bands. */
	float SampleAmbientGustBands(float2 worldPosition,
		AmbientGust gusts[AmbientGustCapacity], uint activeGustCount, WindTuning tuning)
	{
		return Detail::SampleAmbientBands(worldPosition, gusts, activeGustCount, tuning).envelope;
	}

	/** @brief Samples explicit gust bands with one world-space procedural breakup stack. */
	float SampleAmbientGust(float3 worldPosition,
		AmbientGust gusts[AmbientGustCapacity], uint activeGustCount, WindTuning tuning)
	{
		Detail::AmbientBandSample bandSample =
			Detail::SampleAmbientBands(worldPosition.xy, gusts, activeGustCount, tuning);
		if (bandSample.envelope <= 0.0f)
			return 0.5f;

		float noiseScale = max(abs(tuning.gustNoiseScale), Detail::MinimumDivisor);
		float2 broadCoordinate = worldPosition.xy / noiseScale + bandSample.noiseOffset;
		float broadGust = Detail::GradientNoise(broadCoordinate, tuning.broadGustSeed, tuning);
		float detailScaleRatio = max(abs(tuning.detailScaleRatio), Detail::MinimumDivisor);
		float detailCrosswindScaleRatio = max(abs(tuning.detailCrosswindScaleRatio), Detail::MinimumDivisor);
		float2 detailCoordinate = float2(
			broadCoordinate.x / detailScaleRatio + broadCoordinate.y * tuning.turbulenceSkew,
			broadCoordinate.y / detailCrosswindScaleRatio);
		float turbulentGust = Detail::GradientNoise(detailCoordinate, tuning.turbulentGustSeed, tuning);
		float turbulenceStrength = max(tuning.turbulenceStrength, 0.0f);
		float gustNoise = (broadGust + turbulentGust * turbulenceStrength) / (1.0f + turbulenceStrength);
		float normalizedNoise = saturate(gustNoise * 0.5f + 0.5f);
		float contrastLow = min(tuning.contrastLow, tuning.contrastHigh);
		float contrastHigh = max(tuning.contrastLow, tuning.contrastHigh);
		float shapedNoise = Detail::Smoothstep(contrastLow, contrastHigh, normalizedNoise);
		float breakup = lerp(1.0f, shapedNoise, saturate(tuning.gustNoiseAmount));
		return 0.5f + saturate(bandSample.envelope * breakup) * 0.5f;
	}

	/** @brief Samples wind when the base direction and speed are already combined as velocity. */
	WindSample SampleWindVelocity(float3 worldPosition, float3 baseVelocity, WindTuning tuning,
		AmbientGust gusts[AmbientGustCapacity], uint activeGustCount)
	{
		WindSample sample;
		sample.ambientGust = SampleAmbientGust(worldPosition, gusts, activeGustCount, tuning);
		sample.transientImpulse = 0.0f;
		float gustDeviation = sample.ambientGust * 2.0f - 1.0f;
		float gustMultiplier = max(1.0f + gustDeviation * max(tuning.gustAmplitude, 0.0f), 0.0f);
		sample.velocity = baseVelocity * gustMultiplier;
		return sample;
	}

	/** @brief GPU equivalent of the canonical CPU SampleWind implementation. */
	WindSample SampleWind(float3 worldPosition, float3 windDirection, float windSpeed, WindTuning tuning,
		AmbientGust gusts[AmbientGustCapacity], uint activeGustCount)
	{
		float3 baseVelocity = Detail::NormalizeDirection(windDirection) * max(windSpeed, 0.0f);
		return SampleWindVelocity(worldPosition, baseVelocity, tuning, gusts, activeGustCount);
	}

	/** @brief GPU equivalent of the canonical CPU SampleAmbientWind implementation. */
	float3 SampleAmbientWind(float3 worldPosition, float3 windDirection, float windSpeed, WindTuning tuning,
		AmbientGust gusts[AmbientGustCapacity], uint activeGustCount)
	{
		return SampleWind(worldPosition, windDirection, windSpeed, tuning, gusts, activeGustCount).velocity;
	}
}

#endif  // __WIND_FIELD_DEPENDENCY_HLSL__
