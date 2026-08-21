#pragma once

#include <cstdint>
#include <type_traits>

namespace WindField
{
	/** @brief Fixed procedural controls and seeds for stateless ambient weather wind. */
	struct WindTuning
	{
		float gustScale{ 2048.0f };
		float frontAspectRatio{ 4.0f };
		float gustAdvectionBaseSpeed{ 384.0f };
		float gustAdvectionMultiplier{ 1.0f };
		float detailScaleRatio{ 0.38f };
		float detailCrosswindScaleRatio{ 0.55f };
		float turbulenceStrength{ 0.24f };
		float turbulenceSkew{ 0.35f };
		float contrastLow{ 0.30f };
		float contrastHigh{ 0.70f };
		float gustAmplitude{ 0.35f };
		uint32_t broadGustSeed{ 0x2341316Cu };
		uint32_t turbulentGustSeed{ 0x48013EA4u };
		uint32_t gradientSeedMix{ 0x1E3779B9u };
		uint32_t pcgMultiplier{ 1664525u };
		uint32_t pcgIncrement{ 1013904223u };
	};
	static_assert(sizeof(WindTuning) == 64);
	static_assert(std::is_standard_layout_v<WindTuning>);

	/** @brief A point sample of the procedural wind field. */
	struct WindSample
	{
		float3 velocity{};
		float ambientGust{};
	};

	/**
	 * @brief Samples normalized ambient gust pressure from absolute world position and accumulated travel.
	 * @param a_worldPosition Absolute position in Skyrim's Z-up world space.
	 * @param a_gustTravelDistance Accumulated non-negative gust travel distance in world units.
	 * @param a_windDirection Global weather travel direction; its XY projection advects gust fronts.
	 * @param a_tuning Fixed procedural scales and seeds.
	 * @return Deterministic gust pressure in the range [0, 1].
	 */
	[[nodiscard]] float SampleAmbientGust(const float3& a_worldPosition, float a_gustTravelDistance,
		const float3& a_windDirection, const WindTuning& a_tuning) noexcept;

	/**
	 * @brief Samples the ambient weather contribution as a 3D velocity.
	 * @param a_gustTravelDistance Accumulated non-negative gust travel distance in world units.
	 * @return Ambient velocity after the procedural gust multiplier is applied.
	 */
	[[nodiscard]] float3 SampleAmbientWind(const float3& a_worldPosition, float a_gustTravelDistance,
		const float3& a_windDirection, float a_windSpeed, const WindTuning& a_tuning) noexcept;

	/**
	 * @brief Samples the canonical stateless 3D wind field.
	 * @details Future local XYZ sources add to this sample without changing its velocity-shaped API.
	 * @param a_gustTravelDistance Accumulated non-negative gust travel distance in world units.
	 * @return Wind velocity and normalized ambient gust pressure at the requested point and travel phase.
	 */
	[[nodiscard]] WindSample SampleWind(const float3& a_worldPosition, float a_gustTravelDistance,
		const float3& a_windDirection, float a_windSpeed, const WindTuning& a_tuning) noexcept;
}
