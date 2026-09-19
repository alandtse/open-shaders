#pragma once

#include <cstdint>

namespace NeuralRendering
{
	/** @brief Separates a model-tier change from changes that invalidate display-space handoff history. */
	struct TemporalHistoryConfig
	{
		bool adaptive = false;
		std::uint32_t modelResolution = 100;
		std::uint32_t cadence = 0;
		float depthThreshold = 0.05f;
		float colorTolerance = 0.08f;
		std::uint32_t passes = 1;

		bool operator==(const TemporalHistoryConfig&) const = default;

		[[nodiscard]] bool PreservesHandoff(const TemporalHistoryConfig& next) const
		{
			auto sameTier = next;
			sameTier.modelResolution = modelResolution;
			return adaptive && next.adaptive && *this == sameTier;
		}
	};

	/** @brief Tracks the active/previous tier; the renderer may add its two immediate neighbors for bounded prewarm. */
	struct TierResidency
	{
		std::uint32_t current = UINT32_MAX;
		std::uint32_t previous = UINT32_MAX;

		bool Select(std::uint32_t tier, bool retainPrevious)
		{
			const bool changed = current != tier;
			if (changed) {
				previous = current;
				current = tier;
			}
			if (!retainPrevious)
				previous = UINT32_MAX;
			return changed;
		}

		[[nodiscard]] bool Contains(std::uint32_t tier) const
		{
			return tier == current || tier == previous;
		}
	};
}
