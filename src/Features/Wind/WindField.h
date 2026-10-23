#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace WindField
{
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
		float distortionStrength{};
		float distortionScale{ 1.0f };
		float distortionSpeed{};
		float microTurbulenceStrength{ 1.0f };
		float fineBaselineAmplitude{ 0.025f };
		float fineWindCoupling{ 0.35f };
		float fineGustCoupling{ 0.75f };
		float fineFrequency{ 1.0f };
	};
	static_assert(sizeof(WindTuning) == 96);
	static_assert(offsetof(WindTuning, microTurbulenceStrength) == 76);
	static_assert(offsetof(WindTuning, fineBaselineAmplitude) == 80);
	static_assert(std::is_standard_layout_v<WindTuning>);

	/** @brief Integrated world-space transport split into coarse XY and residual ZW offsets. */
	struct FineState
	{
		float4 travel{};
	};
	static_assert(sizeof(FineState) == 16);

	/** @brief Shared slow and detail air-velocity deviations, before material response. */
	struct FineSample
	{
		float3 slowVelocity{};
		float3 detailVelocity{};
	};

	/** @brief One procedural noise-field instance with an orientation fixed for its lifetime. */
	struct Field
	{
		float3 direction{ 1.0f, 0.0f, 0.0f };
		float travelDistance{};
		float3 crosswind{ 0.0f, 1.0f, 0.0f };
		float speed{};
	};
	static_assert(sizeof(Field) == 32);
	static_assert(std::is_standard_layout_v<Field>);

	struct WindSample
	{
		float3 velocity{};
		float ambientGust{};
		float transientImpulse{};
	};

	[[nodiscard]] Field CreateField(const float3& a_direction, float a_speed,
		float a_travelDistance = 0.0f) noexcept;
	void SetFieldSpeed(Field& a_field, float a_speed) noexcept;
	void AdvanceField(Field& a_field, float a_distance) noexcept;

	[[nodiscard]] WindSample SampleField(const float3& a_worldPosition, const Field& a_field,
		const WindTuning& a_tuning) noexcept;
	/** @brief Integrates fine-field transport along the blended ambient wind without resetting its domain. */
	void AdvanceFineState(FineState& a_state, const float3& a_baseVelocity,
		float a_frameTime, const WindTuning& a_tuning) noexcept;
	/** @brief Samples independent fine bands; larger spatial/travel scales mean smaller/faster patches. */
	[[nodiscard]] FineSample SampleFineVelocity(const float3& a_worldPosition, const float3& a_baseVelocity,
		const float3& a_gustVelocity, const FineState& a_state, const WindTuning& a_tuning,
		float a_spatialScale = 1.0f, float a_travelScale = 1.0f) noexcept;
}
