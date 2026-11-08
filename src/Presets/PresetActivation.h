#pragma once

#include "PresetState.h"

#include <map>
#include <set>

namespace PresetActivation
{
	using Footprint = std::map<std::string, nlohmann::json>;

	struct Candidate
	{
		std::string id;
		Footprint settings;
	};

	struct Selection
	{
		std::vector<std::string> enabled;
		std::map<std::string, std::vector<std::string>> blocked;
	};

	/** @brief Whether scene, record or exclusive payloads conflict, independent of Base Settings priority. */
	inline bool Conflicts(const Footprint& first, const Footprint& second)
	{
		for (const auto& [key, value] : first)
			if (const auto other = second.find(key); !key.starts_with("base/") && other != second.end() && (key.starts_with("exclusive/") || value != other->second))
				return true;
		return false;
	}

	/** @brief Preserves enabled presets and admits new non-conflicting presets in stable identifier order. */
	inline Selection Select(std::vector<Candidate> candidates, std::span<const std::string> enabled,
		std::span<const std::string> disabled, std::span<const std::string> known, bool automatic)
	{
		std::ranges::sort(candidates, {}, &Candidate::id);
		Selection result;
		const auto find = [&](const std::string& id) { return std::ranges::find(candidates, id, &Candidate::id); };
		for (const auto& id : enabled)
			if (find(id) != candidates.end() && std::ranges::find(disabled, id) == disabled.end()) {
				bool conflict = false;
				for (const auto& other : result.enabled)
					if (Conflicts(find(id)->settings, find(other)->settings)) {
						result.blocked[id].push_back(other);
						conflict = true;
					}
				if (!conflict)
					result.enabled.push_back(id);
			}
		if (!automatic)
			return result;
		for (const auto& candidate : candidates) {
			if (std::ranges::find(result.enabled, candidate.id) != result.enabled.end() ||
				std::ranges::find(disabled, candidate.id) != disabled.end())
				continue;
			std::vector<std::string> conflicts;
			for (const auto& id : result.enabled)
				if (Conflicts(candidate.settings, find(id)->settings))
					conflicts.push_back(id);
			if (!conflicts.empty())
				result.blocked[candidate.id] = std::move(conflicts);
			else if (std::ranges::find(known, candidate.id) == known.end())
				result.enabled.push_back(candidate.id);
		}
		return result;
	}
}
