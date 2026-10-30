#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ABPrerequisites
{
	/** @brief How many enabling settings a chain follows before giving up. */
	constexpr std::size_t kMaxDepth = 4;

	/** @brief A setting that must change before the subject can be used, and the value it must hold. */
	template <class Setting, class Value>
	struct Step
	{
		const Setting* setting;
		Value required;
	};

	/**
	 * @brief Orders the prerequisites of `setting` that are not met yet, outermost first.
	 *
	 * Follows each setting's enabledBy link up to kMaxDepth levels.
	 * @param findSetting Looks a prerequisite up by its catalogued key and path; nullptr when it is not catalogued.
	 * @param currentValue Reads a setting's current value.
	 * @param requiredValue Builds the value a prerequisite must hold from its 0/1 requirement.
	 * @return The steps to set, in the order a prepare must run them; empty when every prerequisite is already met,
	 *         and nullopt on a cycle, a missing setting or a chain deeper than kMaxDepth.
	 */
	template <class Setting, class Value, class FindSetting, class CurrentValue, class RequiredValue>
	std::optional<std::vector<Step<Setting, Value>>> ResolvePrerequisites(const Setting& setting,
		FindSetting findSetting, CurrentValue currentValue, RequiredValue requiredValue)
	{
		std::vector<Step<Setting, Value>> steps;
		std::vector<const Setting*> visited{ &setting };
		const Setting* current = &setting;
		for (std::size_t depth = 0; depth < kMaxDepth; ++depth) {
			if (current->enabledByValue < 0 || current->enabledByKey.empty())
				break;
			const Setting* prerequisite = findSetting(current->enabledByKey, current->enabledByPath);
			if (!prerequisite)
				return std::nullopt;
			if (std::find(visited.begin(), visited.end(), prerequisite) != visited.end())
				return std::nullopt;
			visited.push_back(prerequisite);
			Value required = requiredValue(*prerequisite, current->enabledByValue);
			if (currentValue(*prerequisite) != required)
				steps.push_back({ prerequisite, std::move(required) });
			current = prerequisite;
		}
		if (current->enabledByValue >= 0 && !current->enabledByKey.empty())
			return std::nullopt;
		std::reverse(steps.begin(), steps.end());
		return steps;
	}
}
