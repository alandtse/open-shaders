#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace WindField
{
	inline constexpr uint32_t kAmbientGustCapacity = 8;

	/** @brief One CPU-authored, world-space ambient gust band shared with GPU consumers. */
	struct alignas(16) AmbientGust
	{
		float2 position{};
		float2 direction{};
		float speed{};
		float length{};
		float width{};
		float strength{};
		float age{};
		float lifetime{};
		uint32_t seed{};
		uint32_t padding{};
	};
	static_assert(sizeof(AmbientGust) == 48);
	static_assert(std::is_standard_layout_v<AmbientGust>);

	/** @brief Shared ambient-band, breakup-noise, and CPU spawning controls. */
	struct WindTuning
	{
		float gustAmplitude{ 0.35f };
		float gustEdgeSoftness{ 0.75f };
		float gustNoiseAmount{ 0.85f };
		float gustNoiseScale{ 2048.0f };
		float detailScaleRatio{ 0.38f };
		float detailCrosswindScaleRatio{ 0.55f };
		float turbulenceStrength{ 0.24f };
		float turbulenceSkew{ 0.35f };
		float contrastLow{ 0.30f };
		float contrastHigh{ 0.70f };
		uint32_t broadGustSeed{ 0x2341316Cu };
		uint32_t turbulentGustSeed{ 0x48013EA4u };
		uint32_t gradientSeedMix{ 0x1E3779B9u };
		uint32_t pcgMultiplier{ 1664525u };
		uint32_t pcgIncrement{ 1013904223u };
		uint32_t maxActiveGusts{ 6u };
		float spawnIntervalMin{ 6.0f };
		float spawnIntervalMax{ 10.0f };
		float gustLengthMin{ 16000.0f };
		float gustLengthMax{ 26000.0f };
		float gustWidthMin{ 3000.0f };
		float gustWidthMax{ 5000.0f };
		float gustSpeedMin{ 384.0f };
		float gustSpeedMax{ 640.0f };
		float gustStrengthMin{ 0.65f };
		float gustStrengthMax{ 1.0f };
		float gustLifetimeMin{ 90.0f };
		float gustLifetimeMax{ 140.0f };
		float gustAdvectionMultiplier{ 1.0f };
		float gustDirectionVariation{ 0.08f };
		float padding[2]{};
	};
	static_assert(sizeof(WindTuning) == 128);
	static_assert(std::is_standard_layout_v<WindTuning>);

	/** @brief A point sample of the shared wind field. */
	struct WindSample
	{
		float3 velocity{};
		float ambientGust{};
		float transientImpulse{};
	};

	/** @brief Samples the soft, bounded sum of active world-space gust bands. */
	[[nodiscard]] float SampleAmbientGustBands(const float2& a_worldPosition,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount,
		const WindTuning& a_tuning) noexcept;

	/** @brief Samples normalized ambient gust pressure from explicit bands and world-space breakup noise. */
	[[nodiscard]] float SampleAmbientGust(const float3& a_worldPosition,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount,
		const WindTuning& a_tuning) noexcept;

	/** @brief Samples the ambient weather contribution as a 3D velocity. */
	[[nodiscard]] float3 SampleAmbientWind(const float3& a_worldPosition, const float3& a_windDirection,
		float a_windSpeed, const WindTuning& a_tuning,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount) noexcept;

	/** @brief Samples the canonical shared 3D wind field. */
	[[nodiscard]] WindSample SampleWind(const float3& a_worldPosition, const float3& a_windDirection,
		float a_windSpeed, const WindTuning& a_tuning,
		const std::array<AmbientGust, kAmbientGustCapacity>& a_gusts, uint32_t a_activeGustCount) noexcept;
}
