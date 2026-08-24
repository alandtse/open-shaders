#include "WindField.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace WindField
{
	namespace
	{
		constexpr float kMinimumDivisor = 1e-4f;
		constexpr float kGradientIntegerScale = 2.0f / 16777215.0f;
		constexpr float kNoiseNormalization = 0.70710678f;

		struct Uint2
		{
			uint32_t x;
			uint32_t y;
		};

		struct Scalar2
		{
			float x;
			float y;
		};

		struct AmbientBandSample
		{
			float envelope{};
			Scalar2 noiseOffset{};
		};

		Uint2 Pcg2D(Uint2 a_value, const WindTuning& a_tuning) noexcept
		{
			a_value.x = a_value.x * a_tuning.pcgMultiplier + a_tuning.pcgIncrement;
			a_value.y = a_value.y * a_tuning.pcgMultiplier + a_tuning.pcgIncrement;
			a_value.x += a_value.y * a_tuning.pcgMultiplier;
			a_value.y += a_value.x * a_tuning.pcgMultiplier;
			a_value.x ^= a_value.x >> 16u;
			a_value.y ^= a_value.y >> 16u;
			a_value.x += a_value.y * a_tuning.pcgMultiplier;
			a_value.y += a_value.x * a_tuning.pcgMultiplier;
			a_value.x ^= a_value.x >> 16u;
			a_value.y ^= a_value.y >> 16u;
			return a_value;
		}

		Scalar2 GetGradient(Uint2 a_latticePosition, uint32_t a_seed, const WindTuning& a_tuning) noexcept
		{
			const Uint2 hash = Pcg2D({ a_latticePosition.x ^ a_seed,
										 a_latticePosition.y ^ (a_seed ^ a_tuning.gradientSeedMix) },
				a_tuning);
			Scalar2 gradient{
				static_cast<float>(hash.x >> 8u) * kGradientIntegerScale - 1.0f,
				static_cast<float>(hash.y >> 8u) * kGradientIntegerScale - 1.0f
			};
			const float inverseLength = 1.0f / std::sqrt(std::max(
												   gradient.x * gradient.x + gradient.y * gradient.y, kMinimumDivisor));
			gradient.x *= inverseLength;
			gradient.y *= inverseLength;
			return gradient;
		}

		float Dot(const Scalar2& a_left, const Scalar2& a_right) noexcept
		{
			return a_left.x * a_right.x + a_left.y * a_right.y;
		}

		float Lerp(float a_start, float a_end, float a_amount) noexcept
		{
			return a_start + (a_end - a_start) * a_amount;
		}

		float GradientNoise(const Scalar2& a_position, uint32_t a_seed, const WindTuning& a_tuning) noexcept
		{
			const Scalar2 latticePosition{ std::floor(a_position.x), std::floor(a_position.y) };
			const Scalar2 offset{ a_position.x - latticePosition.x, a_position.y - latticePosition.y };
			const Uint2 lattice{
				std::bit_cast<uint32_t>(static_cast<int32_t>(latticePosition.x)),
				std::bit_cast<uint32_t>(static_cast<int32_t>(latticePosition.y))
			};

			const float value00 = Dot(GetGradient(lattice, a_seed, a_tuning), offset);
			const float value10 = Dot(
				GetGradient({ lattice.x + 1u, lattice.y }, a_seed, a_tuning), { offset.x - 1.0f, offset.y });
			const float value01 = Dot(
				GetGradient({ lattice.x, lattice.y + 1u }, a_seed, a_tuning), { offset.x, offset.y - 1.0f });
			const float value11 = Dot(GetGradient({ lattice.x + 1u, lattice.y + 1u }, a_seed, a_tuning),
				{ offset.x - 1.0f, offset.y - 1.0f });

			const Scalar2 fade{
				offset.x * offset.x * offset.x * (offset.x * (offset.x * 6.0f - 15.0f) + 10.0f),
				offset.y * offset.y * offset.y * (offset.y * (offset.y * 6.0f - 15.0f) + 10.0f)
			};
			return Lerp(Lerp(value00, value10, fade.x), Lerp(value01, value11, fade.x), fade.y) *
			       kNoiseNormalization;
		}

		float Smoothstep(float a_minimum, float a_maximum, float a_value) noexcept
		{
			const float range = std::max(a_maximum - a_minimum, kMinimumDivisor);
			const float amount = std::clamp((a_value - a_minimum) / range, 0.0f, 1.0f);
			return amount * amount * (3.0f - 2.0f * amount);
		}

		float3 NormalizeDirection(const float3& a_direction) noexcept
		{
			const float lengthSquared = a_direction.x * a_direction.x + a_direction.y * a_direction.y +
			                            a_direction.z * a_direction.z;
			return lengthSquared > 1e-6f ? a_direction / std::sqrt(lengthSquared) : float3{};
		}

		float SampleAmbientGustEnvelope(const float2& a_worldPosition, const AmbientGust& a_gust,
			const WindTuning& a_tuning) noexcept
		{
			const float2 delta = a_worldPosition - a_gust.position;
			const float2 side{ -a_gust.direction.y, a_gust.direction.x };
			const float along = delta.Dot(a_gust.direction);
			const float across = delta.Dot(side);
			const float x = along / std::max(std::abs(a_gust.length), kMinimumDivisor);
			const float y = across / std::max(std::abs(a_gust.width), kMinimumDivisor);
			const float edge = std::clamp(
				(1.0f - (x * x + y * y)) / std::max(a_tuning.gustEdgeSoftness, kMinimumDivisor), 0.0f, 1.0f);
			return edge * edge * (3.0f - 2.0f * edge);
		}

		AmbientBandSample SampleAmbientBands(const float2& a_worldPosition,
			const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount,
			const WindTuning& a_tuning) noexcept
		{
			AmbientBandSample sample{};
			const uint32_t gustCount = std::min(a_activeGustCount, kAmbientGustCapacity);
			for (uint32_t index = 0; index < gustCount; ++index) {
				const auto& gust = a_gusts[index];
				const float contribution = SampleAmbientGustEnvelope(a_worldPosition, gust, a_tuning) *
				                           std::max(gust.strength, 0.0f);
				sample.envelope += contribution;
				sample.noiseOffset.x +=
					static_cast<float>(gust.seed & 0xFFFFu) * (64.0f / 65535.0f) * contribution;
				sample.noiseOffset.y +=
					static_cast<float>(gust.seed >> 16u) * (64.0f / 65535.0f) * contribution;
			}
			if (sample.envelope > kMinimumDivisor) {
				sample.noiseOffset.x /= sample.envelope;
				sample.noiseOffset.y /= sample.envelope;
			}
			sample.envelope = std::clamp(sample.envelope, 0.0f, 1.0f);
			return sample;
		}
	}

	float SampleAmbientGustBands(const float2& a_worldPosition,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount,
		const WindTuning& a_tuning) noexcept
	{
		return SampleAmbientBands(a_worldPosition, a_gusts, a_activeGustCount, a_tuning).envelope;
	}

	float SampleAmbientGust(const float3& a_worldPosition,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount,
		const WindTuning& a_tuning) noexcept
	{
		const AmbientBandSample bandSample =
			SampleAmbientBands({ a_worldPosition.x, a_worldPosition.y }, a_gusts, a_activeGustCount, a_tuning);
		if (bandSample.envelope <= 0.0f)
			return 0.5f;

		const float noiseScale = std::max(std::abs(a_tuning.gustNoiseScale), kMinimumDivisor);
		const Scalar2 broadCoordinate{
			a_worldPosition.x / noiseScale + bandSample.noiseOffset.x,
			a_worldPosition.y / noiseScale + bandSample.noiseOffset.y
		};
		const float broadGust = GradientNoise(broadCoordinate, a_tuning.broadGustSeed, a_tuning);
		const float detailScaleRatio = std::max(std::abs(a_tuning.detailScaleRatio), kMinimumDivisor);
		const float detailCrosswindScaleRatio =
			std::max(std::abs(a_tuning.detailCrosswindScaleRatio), kMinimumDivisor);
		const Scalar2 detailCoordinate{
			broadCoordinate.x / detailScaleRatio + broadCoordinate.y * a_tuning.turbulenceSkew,
			broadCoordinate.y / detailCrosswindScaleRatio
		};
		const float turbulentGust = GradientNoise(detailCoordinate, a_tuning.turbulentGustSeed, a_tuning);
		const float turbulenceStrength = std::max(a_tuning.turbulenceStrength, 0.0f);
		const float gustNoise =
			(broadGust + turbulentGust * turbulenceStrength) / (1.0f + turbulenceStrength);
		const float normalizedNoise = std::clamp(gustNoise * 0.5f + 0.5f, 0.0f, 1.0f);
		const auto [contrastLow, contrastHigh] = std::minmax(a_tuning.contrastLow, a_tuning.contrastHigh);
		const float shapedNoise = Smoothstep(contrastLow, contrastHigh, normalizedNoise);
		const float breakup = Lerp(1.0f, shapedNoise, std::clamp(a_tuning.gustNoiseAmount, 0.0f, 1.0f));
		return 0.5f + std::clamp(bandSample.envelope * breakup, 0.0f, 1.0f) * 0.5f;
	}

	float3 SampleAmbientWind(const float3& a_worldPosition, const float3& a_windDirection,
		float a_windSpeed, const WindTuning& a_tuning,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount) noexcept
	{
		return SampleWind(a_worldPosition, a_windDirection, a_windSpeed, a_tuning, a_gusts, a_activeGustCount).velocity;
	}

	WindSample SampleWind(const float3& a_worldPosition, const float3& a_windDirection,
		float a_windSpeed, const WindTuning& a_tuning,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount) noexcept
	{
		const float ambientGust = SampleAmbientGust(a_worldPosition, a_gusts, a_activeGustCount, a_tuning);
		const float gustDeviation = ambientGust * 2.0f - 1.0f;
		const float gustMultiplier = std::max(1.0f + gustDeviation * std::max(a_tuning.gustAmplitude, 0.0f), 0.0f);
		const float3 ambientWeatherWind =
			NormalizeDirection(a_windDirection) * (std::max(a_windSpeed, 0.0f) * gustMultiplier);
		return { ambientWeatherWind, ambientGust, 0.0f };
	}
}
