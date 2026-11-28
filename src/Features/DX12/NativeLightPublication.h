#pragma once

#include "FullShadowLightConsolidation.h"

#include <bit>
#include <unordered_set>
#include <utility>

namespace DX12Native
{
	inline constexpr uint32_t kDefaultLightObservationFrames = 30;
	inline constexpr uint32_t kMaxLightObservationFrames = 120;
	inline constexpr uint32_t kLightRecordsPerEmitter = 2;
	using PackedNativeLight = std::array<std::array<float, 4>, kLightRecordsPerEmitter>;

	struct NativeLightSettings
	{
		uint32_t unify = kDefaultRuntimeUnificationRadius;
		uint32_t exclude = kDefaultPreparedExclusionRadius;
		bool additive = true;
		bool operator==(const NativeLightSettings&) const = default;
	};

	// Preserve actual shadow-light identities before spatial consolidation.
	// A live placed shadow light replaces its prepared placeholder, not its emission twice.
	template <class Light, class Emitter>
	std::pair<std::vector<Light>, uint32_t> BuildNativeLightInventory(std::span<const Light> prepared,
		std::span<const Emitter> runtime, NativeLightSettings settings, size_t limit)
	{
		std::unordered_set<uint32_t> preparedIDs;
		std::vector<Light> supplemental;
		supplemental.reserve(runtime.size());
		for (const auto& light : prepared)
			if (light.referenceID)
				preparedIDs.insert(light.referenceID);
		std::unordered_set<uint32_t> exactPlaced;
		std::vector<Light> exact, preparedSelection;
		for (const auto& emitter : runtime) {
			if (emitter.shadowLight && emitter.value.runtimeIdentity) {
				exact.push_back(emitter.value);
				if (emitter.placedReference)
					exactPlaced.insert(emitter.placedReference);
			} else if (!emitter.placedReference || !preparedIDs.contains(emitter.placedReference)) {
				supplemental.push_back(emitter.value);
			}
		}
		std::ranges::sort(exact, {}, &Light::runtimeIdentity);
		exact.resize(std::min(exact.size(), limit));
		for (const auto& light : prepared)
			if (!exactPlaced.contains(light.referenceID) && preparedSelection.size() < limit - exact.size())
				preparedSelection.push_back(light);
		auto lights = ConsolidateFullShadowLights<Light>(preparedSelection, supplemental,
			settings.unify, settings.exclude, settings.additive);
		const auto preparedCount = uint32_t(preparedSelection.size());
		lights.insert(lights.begin() + preparedCount, exact.begin(), exact.end());
		lights.resize(std::min(lights.size(), limit));
		return { std::move(lights), preparedCount };
	}

	/** @brief Frame-wrap-safe periodic observation; zero is sanitized to one. */
	constexpr bool NativeLightObservationDue(uint32_t frame, uint32_t previous, uint32_t interval)
	{
		return uint32_t(frame - previous) >= std::clamp(interval, 1u, kMaxLightObservationFrames);
	}

	template <class Light>
	bool SameNativeLightInventory(std::span<const Light> a, std::span<const Light> b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
			return SamePreparedShadowLight(x, y) && x.runtimeIdentity == y.runtimeIdentity && x.unifiedCount == y.unifiedCount;
		});
	}

	/** @brief Exact existing RayQuery t2 ABI, independent of frame and quality. */
	template <class Light>
	PackedNativeLight PackNativeLight(const Light& light)
	{
		const float strength = std::max(light.color.x, 0.0f) * 0.2126f +
		                       std::max(light.color.y, 0.0f) * 0.7152f +
		                       std::max(light.color.z, 0.0f) * 0.0722f;
		return { { { light.position.x, light.position.y, light.position.z, light.radius },
			{ strength * std::max(light.fade, 0.0f), light.emitterRadius * light.emitterRadius,
				std::max(light.emitterRadius, 0.0f), std::bit_cast<float>(light.referenceID) } } };
	}
}
