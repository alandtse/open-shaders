#ifndef __WIND_GUST_DEPENDENCY_HLSL__
#define __WIND_GUST_DEPENDENCY_HLSL__

#include "Random.hlsli"

namespace WindGust
{
	namespace Tuning
	{
		static const float GustScale = 2048.0f;
		static const float FrontAspectRatio = 4.0f;
		static const float AdvectionUnitsPerSecond = 384.0f;
		static const float DetailScaleRatio = 0.38f;
		static const float DetailCrosswindScaleRatio = 0.55f;
		static const float TurbulenceStrength = 0.24f;
		static const float TurbulenceSkew = 0.35f;
		static const float ContrastLow = 0.30f;
		static const float ContrastHigh = 0.70f;
	}

	namespace Detail
	{
		float2 GetGradient(uint2 latticePosition, uint seed)
		{
			uint2 hash = Random::pcg2d(latticePosition ^ uint2(seed, seed ^ 0x1E3779B9u));
			float2 gradient = float2(hash >> 8u) * (2.0f / 16777215.0f) - 1.0f;
			return gradient * rsqrt(max(dot(gradient, gradient), 1e-4f));
		}

		// A 2D lattice needs four hashes rather than the eight used by the existing 3D Perlin utility.
		float GradientNoise(float2 position, uint seed)
		{
			float2 latticePosition = floor(position);
			float2 offset = position - latticePosition;
			uint2 lattice = asuint(int2(latticePosition));

			float value00 = dot(GetGradient(lattice, seed), offset);
			float value10 = dot(GetGradient(lattice + uint2(1u, 0u), seed), offset - float2(1.0f, 0.0f));
			float value01 = dot(GetGradient(lattice + uint2(0u, 1u), seed), offset - float2(0.0f, 1.0f));
			float value11 = dot(GetGradient(lattice + uint2(1u, 1u), seed), offset - float2(1.0f, 1.0f));

			float2 fade = offset * offset * offset * (offset * (offset * 6.0f - 15.0f) + 10.0f);
			return lerp(lerp(value00, value10, fade.x), lerp(value01, value11, fade.x), fade.y) * 0.70710678f;
		}
	}

	/**
	 * @brief Samples normalized gust strength from an absolute Skyrim world position.
	 * @param worldPosition Absolute world position in Skyrim's Z-up coordinate system.
	 * @param windDirection Global wind travel direction on the horizontal XY plane.
	 * @param windSpeed Non-negative normalized wind intensity controlling advection speed.
	 * @param time Monotonic animation time in seconds.
	 * @return Coherent gust intensity in the range [0, 1].
	 */
	float SampleWindGust(float3 worldPosition, float2 windDirection, float windSpeed, float time)
	{
		float directionLengthSquared = dot(windDirection, windDirection);
		float2 direction = directionLengthSquared > 1e-6f ?
		                       windDirection * rsqrt(directionLengthSquared) :
		                       float2(1.0f, 0.0f);
		float2 crosswindDirection = float2(-direction.y, direction.x);

		float alongWind = dot(worldPosition.xy, direction);
		float acrossWind = dot(worldPosition.xy, crosswindDirection);
		float advection = time * max(windSpeed, 0.0f) * Tuning::AdvectionUnitsPerSecond;
		float2 frontCoordinate = float2(
			(alongWind - advection) / Tuning::GustScale,
			acrossWind / (Tuning::GustScale * Tuning::FrontAspectRatio));

		float broadGust = Detail::GradientNoise(frontCoordinate, 0x2341316Cu);
		float2 detailCoordinate = float2(
			frontCoordinate.x / Tuning::DetailScaleRatio + frontCoordinate.y * Tuning::TurbulenceSkew,
			frontCoordinate.y / Tuning::DetailCrosswindScaleRatio);
		float turbulentGust = Detail::GradientNoise(detailCoordinate, 0x48013EA4u);

		float gustNoise = (broadGust + turbulentGust * Tuning::TurbulenceStrength) / (1.0f + Tuning::TurbulenceStrength);
		float normalizedGust = saturate(gustNoise * 0.5f + 0.5f);
		return smoothstep(Tuning::ContrastLow, Tuning::ContrastHigh, normalizedGust);
	}
}

#endif  // __WIND_GUST_DEPENDENCY_HLSL__
