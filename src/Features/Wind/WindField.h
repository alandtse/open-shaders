#pragma once

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
	static_assert(std::is_standard_layout_v<WindTuning>);

	/** Integrated world-space transport supplied by Open Wind. */
	struct FineState
	{
		float4 travel{};
	};
	static_assert(sizeof(FineState) == 16);

	/** One procedural noise-field instance with an orientation fixed for its lifetime. */
	struct Field
	{
		float3 direction{ 1.0f, 0.0f, 0.0f };
		float travelDistance{};
		float3 crosswind{ 0.0f, 1.0f, 0.0f };
		float speed{};
	};
	static_assert(sizeof(Field) == 32);
	static_assert(std::is_standard_layout_v<Field>);

}
