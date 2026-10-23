#include "WindField.h"

#include "Utils/MathUtils.h"

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
		constexpr float kMicroSlowSpatialScale = 768.0f;
		constexpr float kMicroDetailSpatialScale = 128.0f;
		constexpr float kMicroMaximumStrength = 4.0f;
		constexpr float kMicroCrosswindSlope = 0.06992681f;
		constexpr float kMicroCrosswindFraction = 0.07f;
		constexpr float kMicroCalmSpeed = 0.02f;
		constexpr float kMicroMinimumSpeed = 1e-5f;
		constexpr float kMicroMaximumNegativeResponse = 0.8f;
		constexpr float kMicroBandWindShare = 0.5f;
		constexpr float kMicroMinimumTransportSpeed = 128.0f;
		constexpr float kMicroWindTransportScale = 512.0f;
		constexpr uint32_t kMicroNoiseMask = 255u;
		constexpr float kMicroNoisePeriod = 256.0f;
		constexpr uint32_t kMicroSlowSeed = 0xA511E9B3u;
		constexpr uint32_t kMicroDetailSeed = 0x63D83595u;
		constexpr uint32_t kMicroCrosswindSeed = 0xB5297A4Du;

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

		float GradientNoise(const Scalar2& a_position, uint32_t a_seed, const WindTuning& a_tuning,
			uint32_t a_latticeMask = 0xFFFFFFFFu) noexcept
		{
			const Scalar2 latticePosition{ std::floor(a_position.x), std::floor(a_position.y) };
			const Scalar2 offset{ a_position.x - latticePosition.x, a_position.y - latticePosition.y };
			const Uint2 lattice{
				std::bit_cast<uint32_t>(static_cast<int32_t>(latticePosition.x)),
				std::bit_cast<uint32_t>(static_cast<int32_t>(latticePosition.y))
			};

			const float value00 = Dot(GetGradient({ lattice.x & a_latticeMask, lattice.y & a_latticeMask }, a_seed, a_tuning), offset);
			const float value10 = Dot(
				GetGradient({ (lattice.x + 1u) & a_latticeMask, lattice.y & a_latticeMask }, a_seed, a_tuning), { offset.x - 1.0f, offset.y });
			const float value01 = Dot(
				GetGradient({ lattice.x & a_latticeMask, (lattice.y + 1u) & a_latticeMask }, a_seed, a_tuning), { offset.x, offset.y - 1.0f });
			const float value11 = Dot(GetGradient({ (lattice.x + 1u) & a_latticeMask, (lattice.y + 1u) & a_latticeMask }, a_seed, a_tuning),
				{ offset.x - 1.0f, offset.y - 1.0f });

			const Scalar2 fade{
				offset.x * offset.x * offset.x * (offset.x * (offset.x * 6.0f - 15.0f) + 10.0f),
				offset.y * offset.y * offset.y * (offset.y * (offset.y * 6.0f - 15.0f) + 10.0f)
			};
			return std::lerp(std::lerp(value00, value10, fade.x), std::lerp(value01, value11, fade.x), fade.y) *
			       kNoiseNormalization;
		}

		Scalar2 SampleFineBand(const float3& a_position, const FineState& a_state, float a_scale,
			float a_spatialScale, float a_travelScale, uint32_t a_seed, const WindTuning& a_tuning) noexcept
		{
			const float spatialScale = std::max(a_spatialScale, kMinimumDivisor) / a_scale;
			const float travelScale = std::max(a_travelScale, 0.0f) * spatialScale;
			Scalar2 coarse{ a_state.travel.x * travelScale, a_state.travel.y * travelScale };
			coarse.x -= std::floor(coarse.x / kMicroNoisePeriod) * kMicroNoisePeriod;
			coarse.y -= std::floor(coarse.y / kMicroNoisePeriod) * kMicroNoisePeriod;
			const Scalar2 coordinate{
				a_position.x * spatialScale - coarse.x - a_state.travel.z * travelScale,
				a_position.y * spatialScale - coarse.y - a_state.travel.w * travelScale
			};
			return {
				std::clamp(2.0f * GradientNoise(coordinate, a_seed, a_tuning, kMicroNoiseMask), -1.0f, 1.0f),
				std::clamp(2.0f * GradientNoise({ coordinate.x + 17.0f, coordinate.y + 31.0f },
									  a_seed ^ kMicroCrosswindSeed, a_tuning, kMicroNoiseMask),
					-1.0f, 1.0f)
			};
		}

		float3 ResolveFineBand(const Scalar2& a_noise, const float3& a_direction, float a_speed, float a_amplitude) noexcept
		{
			const float budget = a_speed * kMicroBandWindShare;
			const float along = a_amplitude * a_noise.x;
			const float across = a_amplitude * kMicroCrosswindFraction * a_noise.y;
			float responseScale = along < -kMicroMaximumNegativeResponse * budget ?
			                          kMicroMaximumNegativeResponse * budget / -along :
			                          1.0f;
			const float crosswindDivisor = std::abs(across) - kMicroCrosswindSlope * along;
			if (crosswindDivisor > 0.0f)
				responseScale = std::min(responseScale, kMicroCrosswindSlope * budget / crosswindDivisor);
			const float3 crosswind{ -a_direction.y, a_direction.x, 0.0f };
			return (a_direction * along + crosswind * across) * responseScale;
		}

		using Util::Smoothstep;

		float SampleAmbientGust(const float3& a_worldPosition, const Field& a_field,
			const WindTuning& a_tuning) noexcept
		{
			const Scalar2 worldPosition{ a_worldPosition.x, a_worldPosition.y };
			const Scalar2 direction{ a_field.direction.x, a_field.direction.y };
			const Scalar2 crosswind{ a_field.crosswind.x, a_field.crosswind.y };
			const float gustScale = std::max(std::abs(a_tuning.gustScale), kMinimumDivisor);
			const float frontAspectRatio = std::max(std::abs(a_tuning.frontAspectRatio), kMinimumDivisor);
			Scalar2 frontCoordinate{
				(Dot(worldPosition, direction) - std::max(a_field.travelDistance, 0.0f)) / gustScale,
				Dot(worldPosition, crosswind) / (gustScale * frontAspectRatio)
			};
			const float distortionStrength = std::max(a_tuning.distortionStrength, 0.0f);
			if (distortionStrength > 0.0f) {
				const float distortionScale = std::max(std::abs(a_tuning.distortionScale), kMinimumDivisor);
				const float distortionTime = std::max(a_field.travelDistance, 0.0f) / gustScale *
				                             std::max(a_tuning.distortionSpeed, 0.0f);
				const Scalar2 distortionCoordinate{
					frontCoordinate.x / distortionScale + distortionTime * 0.37f,
					frontCoordinate.y / distortionScale + distortionTime * -0.23f
				};
				const Scalar2 distortion{
					GradientNoise(distortionCoordinate, a_tuning.broadGustSeed ^ 0x68BC21EBu, a_tuning),
					GradientNoise({ distortionCoordinate.y + 17.0f, distortionCoordinate.x + 29.0f },
						a_tuning.turbulentGustSeed ^ 0x02E5BE93u, a_tuning)
				};
				frontCoordinate.x += distortion.x * distortionStrength;
				frontCoordinate.y += distortion.y * distortionStrength;
			}
			const float broadGust = GradientNoise(frontCoordinate, a_tuning.broadGustSeed, a_tuning);
			const float detailScaleRatio = std::max(std::abs(a_tuning.detailScaleRatio), kMinimumDivisor);
			const float detailCrosswindScaleRatio =
				std::max(std::abs(a_tuning.detailCrosswindScaleRatio), kMinimumDivisor);
			const Scalar2 detailCoordinate{
				frontCoordinate.x / detailScaleRatio + frontCoordinate.y * a_tuning.turbulenceSkew,
				frontCoordinate.y / detailCrosswindScaleRatio
			};
			const float turbulentGust = GradientNoise(detailCoordinate, a_tuning.turbulentGustSeed, a_tuning);
			const float turbulenceStrength = std::max(a_tuning.turbulenceStrength, 0.0f);
			const float gustNoise =
				(broadGust + turbulentGust * turbulenceStrength) / (1.0f + turbulenceStrength);
			const float normalizedGust = gustNoise * 0.5f + 0.5f;
			const auto [contrastLow, contrastHigh] = std::minmax(a_tuning.contrastLow, a_tuning.contrastHigh);
			return Smoothstep(contrastLow, contrastHigh, normalizedGust);
		}
	}

	Field CreateField(const float3& a_direction, float a_speed, float a_travelDistance) noexcept
	{
		const float horizontalLengthSquared = a_direction.x * a_direction.x + a_direction.y * a_direction.y;
		const float inverseLength = horizontalLengthSquared > 1e-6f ? 1.0f / std::sqrt(horizontalLengthSquared) : 1.0f;
		const float3 direction = horizontalLengthSquared > 1e-6f ?
		                             float3{ a_direction.x * inverseLength, a_direction.y * inverseLength, 0.0f } :
		                             float3{ 1.0f, 0.0f, 0.0f };
		return {
			direction,
			std::isfinite(a_travelDistance) ? std::max(a_travelDistance, 0.0f) : 0.0f,
			{ -direction.y, direction.x, 0.0f },
			std::isfinite(a_speed) ? std::max(a_speed, 0.0f) : 0.0f
		};
	}

	void SetFieldSpeed(Field& a_field, float a_speed) noexcept
	{
		a_field.speed = std::isfinite(a_speed) ? std::max(a_speed, 0.0f) : 0.0f;
	}

	void AdvanceField(Field& a_field, float a_distance) noexcept
	{
		if (std::isfinite(a_distance))
			a_field.travelDistance = std::max(a_field.travelDistance + a_distance, 0.0f);
	}

	WindSample SampleField(const float3& a_worldPosition, const Field& a_field,
		const WindTuning& a_tuning) noexcept
	{
		const float ambientGust = SampleAmbientGust(a_worldPosition, a_field, a_tuning);
		const float gustMultiplier = std::max(
			1.0f + (ambientGust * 2.0f - 1.0f) * std::max(a_tuning.gustAmplitude, 0.0f), 0.0f);
		return { a_field.direction * (a_field.speed * gustMultiplier), ambientGust, 0.0f };
	}

	void AdvanceFineState(FineState& a_state, const float3& a_baseVelocity,
		float a_frameTime, const WindTuning& a_tuning) noexcept
	{
		const float speed = std::hypot(a_baseVelocity.x, a_baseVelocity.y);
		if (!std::isfinite(speed) || speed <= kMicroMinimumSpeed ||
			!std::isfinite(a_frameTime) || a_frameTime <= 0.0f ||
			!std::isfinite(a_tuning.fineFrequency) || a_tuning.fineFrequency <= 0.0f)
			return;
		const double distance = (kMicroMinimumTransportSpeed + kMicroWindTransportScale * speed) *
		                        static_cast<double>(a_frameTime) * a_tuning.fineFrequency;
		const double x = static_cast<double>(a_state.travel.x) + a_state.travel.z + a_baseVelocity.x / speed * distance;
		const double y = static_cast<double>(a_state.travel.y) + a_state.travel.w + a_baseVelocity.y / speed * distance;
		const float coarseX = static_cast<float>(std::trunc(x / kMicroSlowSpatialScale) * kMicroSlowSpatialScale);
		const float coarseY = static_cast<float>(std::trunc(y / kMicroSlowSpatialScale) * kMicroSlowSpatialScale);
		a_state.travel = float4{ coarseX, coarseY, static_cast<float>(x - coarseX), static_cast<float>(y - coarseY) };
	}

	FineSample SampleFineVelocity(const float3& a_worldPosition, const float3& a_baseVelocity,
		const float3& a_gustVelocity, const FineState& a_state, const WindTuning& a_tuning,
		float a_spatialScale, float a_travelScale) noexcept
	{
		FineSample sample;
		const float strength = std::clamp(a_tuning.microTurbulenceStrength, 0.0f, kMicroMaximumStrength);
		const float baseSpeed = std::hypot(a_baseVelocity.x, a_baseVelocity.y);
		const float3 ambientVelocity = a_baseVelocity + a_gustVelocity;
		const float speed = std::hypot(ambientVelocity.x, ambientVelocity.y);
		if (strength <= 0.0f || speed <= kMicroMinimumSpeed)
			return sample;
		const float3 direction{ ambientVelocity.x / speed, ambientVelocity.y / speed, 0.0f };
		const float3 baseDirection = baseSpeed > kMicroMinimumSpeed ?
		                                 float3{ a_baseVelocity.x / baseSpeed, a_baseVelocity.y / baseSpeed, 0.0f } :
		                                 direction;
		const float positiveGust = std::max(a_gustVelocity.x * baseDirection.x + a_gustVelocity.y * baseDirection.y, 0.0f);
		const float amplitude = strength * speed / (speed + kMicroCalmSpeed) *
		                        (std::max(a_tuning.fineBaselineAmplitude, 0.0f) +
									baseSpeed * std::max(a_tuning.fineWindCoupling, 0.0f) +
									positiveGust * std::max(a_tuning.fineGustCoupling, 0.0f));
		const auto slow = SampleFineBand(a_worldPosition, a_state, kMicroSlowSpatialScale,
			a_spatialScale, a_travelScale, a_tuning.broadGustSeed ^ kMicroSlowSeed, a_tuning);
		const auto detail = SampleFineBand(a_worldPosition, a_state, kMicroDetailSpatialScale,
			a_spatialScale, a_travelScale, a_tuning.turbulentGustSeed ^ kMicroDetailSeed, a_tuning);
		sample.slowVelocity = ResolveFineBand(slow, direction, speed, amplitude);
		sample.detailVelocity = ResolveFineBand(detail, direction, speed, amplitude);
		return sample;
	}
}
