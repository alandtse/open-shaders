#ifndef __WIND_FIELD_DEPENDENCY_HLSL__
#define __WIND_FIELD_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/TransientWindImpulse.hlsli"
#include "Common/WindFieldTypes.hlsli"

namespace WindField
{
	WindSample ResolveSample(Components components, float gustInfluence, float transientInfluence)
	{
		float3 ambientVelocity = components.baseAmbientVelocity +
		                         components.gustVelocity * max(gustInfluence, 0.0f);
		float3 transientVelocity = components.transientVelocity * max(transientInfluence, 0.0f);
		WindSample sample;
		sample.velocity = ambientVelocity + transientVelocity;
		sample.ambientGust = components.ambientGust;
		sample.ambientTurbulence = components.ambientTurbulence;
		sample.transientImpulse = components.transientImpulse;
		return sample;
	}

	namespace Detail
	{
		static const float MinimumDivisor = EPSILON_WIND_GEOMETRY;

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

		float SampleAmbientGust(float3 worldPosition, Field field, WindTuning tuning,
			out float turbulentGust)
		{
			float gustScale = max(abs(tuning.gustScale), MinimumDivisor);
			float frontAspectRatio = max(abs(tuning.frontAspectRatio), MinimumDivisor);
			float2 frontCoordinate = float2(
				(dot(worldPosition.xy, field.direction.xy) - max(field.travelDistance, 0.0f)) / gustScale,
				dot(worldPosition.xy, field.crosswind.xy) / (gustScale * frontAspectRatio));
			float distortionStrength = max(tuning.distortionStrength, 0.0f);
			if (distortionStrength > 0.0f) {
				float distortionScale = max(abs(tuning.distortionScale), MinimumDivisor);
				float distortionTime = max(field.travelDistance, 0.0f) / gustScale *
				                       max(tuning.distortionSpeed, 0.0f);
				float2 distortionCoordinate = frontCoordinate / distortionScale +
				                              float2(distortionTime * 0.37f, distortionTime * -0.23f);
				float2 distortion = float2(
					GradientNoise(distortionCoordinate, tuning.broadGustSeed ^ 0x68BC21EBu, tuning),
					GradientNoise(distortionCoordinate.yx + float2(17.0f, 29.0f),
						tuning.turbulentGustSeed ^ 0x02E5BE93u, tuning));
				frontCoordinate += distortion * distortionStrength;
			}
			float broadGust = GradientNoise(frontCoordinate, tuning.broadGustSeed, tuning);
			float detailScaleRatio = max(abs(tuning.detailScaleRatio), MinimumDivisor);
			float detailCrosswindScaleRatio = max(abs(tuning.detailCrosswindScaleRatio), MinimumDivisor);
			float2 detailCoordinate = float2(
				frontCoordinate.x / detailScaleRatio + frontCoordinate.y * tuning.turbulenceSkew,
				frontCoordinate.y / detailCrosswindScaleRatio);
			turbulentGust = GradientNoise(detailCoordinate, tuning.turbulentGustSeed, tuning);
			float turbulenceStrength = max(tuning.turbulenceStrength, 0.0f);
			float normalizedGust =
				(broadGust + turbulentGust * turbulenceStrength) / (1.0f + turbulenceStrength) * 0.5f + 0.5f;
			return Smoothstep(min(tuning.contrastLow, tuning.contrastHigh),
				max(tuning.contrastLow, tuning.contrastHigh), normalizedGust);
		}
	}

	WindSample SampleField(float3 worldPosition, Field field, WindTuning tuning)
	{
		WindSample sample;
		sample.ambientGust = Detail::SampleAmbientGust(
			worldPosition, field, tuning, sample.ambientTurbulence);
		sample.transientImpulse = 0.0f;
		float gustMultiplier = max(1.0f + (sample.ambientGust * 2.0f - 1.0f) * max(tuning.gustAmplitude, 0.0f), 0.0f);
		sample.velocity = field.direction * (max(field.speed, 0.0f) * gustMultiplier);
		return sample;
	}

	namespace Detail
	{
		TransientImpulseSample SampleTransientImpulses(
			float3 worldPosition,
			TransientWindSource impulses[TransientImpulseCapacity],
			uint impulseCount)
		{
			TransientImpulseSample sample;
			sample.velocity = 0.0f;
			sample.intensity = 0.0f;
			[loop] for (uint index = 0u; index < impulseCount; ++index)
			{
				TransientImpulseSample impulseSample =
					SampleTransientImpulse(worldPosition, impulses[index]);
				sample.velocity += impulseSample.velocity;
				sample.intensity = max(sample.intensity, impulseSample.intensity);
			}
			return sample;
		}

		WindSample ResolveWithTransient(
			Components components, TransientImpulseSample transientSample,
			float gustInfluence, float transientInfluence)
		{
			components.transientVelocity = transientSample.velocity;
			components.transientImpulse = transientSample.intensity;
			return ResolveSample(components, gustInfluence, transientInfluence);
		}

		Components SampleBlendedComponents(
			float3 worldPosition, Field currentField, Field previousField, float blend)
		{
			WindSample currentSample = SampleField(worldPosition, currentField, SharedData::WindFieldTuning);
			float3 currentBaseVelocity = currentField.direction * max(currentField.speed, 0.0f);
			Components components;
			if (blend >= 1.0f) {
				components.baseAmbientVelocity = currentBaseVelocity;
				components.gustVelocity = currentSample.velocity - currentBaseVelocity;
				components.ambientGust = currentSample.ambientGust;
				components.ambientTurbulence = currentSample.ambientTurbulence;
			} else {
				WindSample previousSample = SampleField(worldPosition, previousField, SharedData::WindFieldTuning);
				float3 previousBaseVelocity = previousField.direction * max(previousField.speed, 0.0f);
				float transition = saturate(blend);
				components.baseAmbientVelocity = lerp(previousBaseVelocity, currentBaseVelocity, transition);
				components.gustVelocity = lerp(previousSample.velocity, currentSample.velocity, transition) -
				                          components.baseAmbientVelocity;
				components.ambientGust =
					lerp(previousSample.ambientGust, currentSample.ambientGust, transition);
				components.ambientTurbulence = lerp(
					previousSample.ambientTurbulence, currentSample.ambientTurbulence, transition);
			}
			components.transientVelocity = 0.0f;
			components.transientImpulse = 0.0f;
			return components;
		}
	}

	Components SampleCurrentComponents(float3 worldPosition)
	{
		return Detail::SampleBlendedComponents(
			worldPosition, SharedData::WindFieldCurrent, SharedData::WindFieldTransition,
			SharedData::WindFieldTransitionData.x);
	}

	Components SamplePreviousComponents(float3 worldPosition)
	{
		return Detail::SampleBlendedComponents(
			worldPosition, SharedData::WindFieldPrevious, SharedData::WindFieldPreviousTransition,
			SharedData::WindFieldTransitionData.y);
	}

	TransientImpulseSample SampleCurrentTransientImpulses(float3 worldPosition)
	{
		return Detail::SampleTransientImpulses(
			worldPosition, SharedData::WindFieldTransientImpulses,
			SharedData::WindFieldActiveCounts.x);
	}

	TransientImpulseSample SamplePreviousTransientImpulses(float3 worldPosition)
	{
		return Detail::SampleTransientImpulses(
			worldPosition, SharedData::WindFieldPreviousTransientImpulses,
			SharedData::WindFieldActiveCounts.y);
	}

	WindSample SampleCurrent(float3 worldPosition, float gustInfluence, float transientInfluence)
	{
		return Detail::ResolveWithTransient(
			SampleCurrentComponents(worldPosition), SampleCurrentTransientImpulses(worldPosition),
			gustInfluence, transientInfluence);
	}

}

#endif  // __WIND_FIELD_DEPENDENCY_HLSL__
