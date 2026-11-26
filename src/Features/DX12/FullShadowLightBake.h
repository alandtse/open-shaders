#pragma once

#include "FullShadowLightConsolidation.h"

#include <memory>
#include <unordered_map>

namespace DX12Native
{
	inline constexpr uint32_t kLightBakeRuns = 2;
	inline constexpr uint32_t kLightBakeSamplesPerRun = 4;
	inline constexpr uint32_t kLightBakeSampleIntervalFrames = 8;
	inline constexpr uint32_t kLightBakeSamples = kLightBakeRuns * kLightBakeSamplesPerRun;

	/** @brief Finite cell-local analysis of consolidated output. Worker-only, no engine objects. */
	template <class Light>
	class FullShadowLightBake
	{
	public:
		using Inventory = std::vector<Light>;
		bool Complete() const { return samples == kLightBakeSamples; }
		uint32_t CompletedRuns() const { return samples / kLightBakeSamplesPerRun; }

		std::shared_ptr<const Inventory> Observe(std::span<const Light> prepared, std::span<const Light> runtime,
			uint32_t unify, uint32_t exclude, bool additive, bool settled)
		{
			if (Complete())
				return baked;
			auto combined = ConsolidateFullShadowLights<Light>(prepared, runtime, unify, exclude, additive);
			const bool sameBase = prepared.size() == base.size() &&
				std::equal(prepared.begin(), prepared.end(), base.begin(), SamePreparedShadowLight<Light>);
			if (!settled || !sameBase || unify != lastUnify || exclude != lastExclude || additive != lastAdditive) {
				samples = 0;
				base.clear();
				stable.clear();
			}
			if (!settled)
				return std::make_shared<const Inventory>(std::move(combined));
			if (!samples) {
				base.assign(prepared.begin(), prepared.end());
				stable = combined;
				lastUnify = unify;
				lastExclude = exclude;
				lastAdditive = additive;
			} else {
				// Prepared records occupy a fixed prefix; unstable compounding
				// reverts to the original prepared record. Runtime groups must
				// have identical aggregate properties and identity in every sample.
				for (size_t i = 0; i < base.size(); ++i)
					if (!Same(stable[i], combined[i]))
						stable[i] = base[i];
				std::unordered_map<uint64_t, const Light*> current;
				for (size_t i = base.size(); i < combined.size(); ++i)
					current.try_emplace(combined[i].runtimeIdentity, &combined[i]);
				auto tail = stable.begin() + static_cast<typename Inventory::difference_type>(base.size());
				stable.erase(std::remove_if(tail, stable.end(), [&](const Light& candidate) {
					const auto it = current.find(candidate.runtimeIdentity);
					return it == current.end() || !Same(candidate, *it->second);
				}), stable.end());
			}
			++samples;
			if (!Complete())
				return std::make_shared<const Inventory>(std::move(combined));

			baked = std::make_shared<const Inventory>(std::move(stable));
			Inventory{}.swap(base);
			Inventory{}.swap(stable);
			return baked;
		}

	private:
		static bool Same(const Light& a, const Light& b)
		{
			return SamePreparedShadowLight(a, b) && a.runtimeIdentity == b.runtimeIdentity && a.unifiedCount == b.unifiedCount;
		}
		uint32_t samples = 0, lastUnify = 0, lastExclude = 0;
		bool lastAdditive = false;
		Inventory base, stable;
		std::shared_ptr<const Inventory> baked;
	};
}
