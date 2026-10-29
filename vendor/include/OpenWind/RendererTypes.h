#pragma once

#include "OpenWind/TransientWindImpulse.h"
#include "OpenWind/TerrainShelter.h"
#include "OpenWind/ObstacleShelter.h"
#include "OpenWind/WindField.h"
#include "OpenWind/WindRuntimeState.h"
#include "OpenWind/WindSettings.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace OpenWind
{
	inline constexpr uint32_t kRendererRevision = 1;

	/** One authoritative update and its previous rendered state; no GPU resource ownership crosses the bridge. */
	struct RendererFrame
	{
		uint32_t structSize = sizeof(RendererFrame);
		uint32_t revision = kRendererRevision;
		uint64_t frameId{};
		uint64_t resetId{};
		WindSettings settings{};
		WindRuntimeState controls{};
		WindField::WindTuning tuning{}, previousTuning{};
		WindField::FineState fine{}, previousFine{}, grassFineTransport{};
		WindField::Field current{}, previous{}, transition{}, previousTransition{};
		float3 ambientVelocity{}, selectedVelocity{}, previousSelectedVelocity{};
		float frameTime{};
		float ambientSpeed{};
		float advectionSpeed{};
		float gustTravelDistance{};
		float previousGustTravelDistance{};
		float travelDelta{};
		float transitionBlend = 1.0f;
		float previousTransitionBlend = 1.0f;
		float selectedSpeed{};
		uint32_t initialized{};
		std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> transients{};
		std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> previousTransients{};
		uint32_t transientCount{};
		uint32_t previousTransientCount{};
		TerrainShelter::Frame shelter{};
		ObstacleShelter::Frame obstacles{};
	};
	static_assert(std::is_standard_layout_v<RendererFrame>);
	static_assert(std::is_trivially_copyable_v<RendererFrame>);
}
