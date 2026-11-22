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

	/** @brief Gives the most recently confirmed conflict winner first choice on subsequent startups. */
	inline void RecordConflictChoice(nlohmann::json& state, const std::string& id)
	{
		auto preferred = PresetState::ReadIds(state, PresetState::kPreferredScenePacksKey);
		std::erase(preferred, id);
		preferred.insert(preferred.begin(), id);
		state[PresetState::kPreferredScenePacksKey] = std::move(preferred);
	}

	/** @brief Resolves installed presets using confirmed conflict choices and stable automatic defaults. */
	inline Selection Select(std::vector<Candidate> candidates, std::span<const std::string> enabled,
		std::span<const std::string> disabled, bool automatic, std::span<const std::string> preferred = {})
	{
		Selection result;
		std::vector<const Candidate*> accepted;
		const auto consider = [&](const Candidate& candidate) {
			if (std::ranges::find(disabled, candidate.id) != disabled.end())
				return;
			std::vector<std::string> conflicts;
			for (const auto* other : accepted)
				if (Conflicts(candidate.settings, other->settings))
					conflicts.push_back(other->id);
			if (conflicts.empty()) {
				accepted.push_back(&candidate);
				result.enabled.push_back(candidate.id);
			} else {
				result.blocked.emplace(candidate.id, std::move(conflicts));
			}
		};
		if (automatic)
			std::ranges::sort(candidates, {}, &Candidate::id);
		for (const auto& id : automatic ? preferred : enabled)
			if (const auto it = std::ranges::find(candidates, id, &Candidate::id); it != candidates.end())
				consider(*it);
		if (automatic) {
			for (const auto& candidate : candidates)
				if (std::ranges::find(preferred, candidate.id) == preferred.end())
					consider(candidate);
		}
		return result;
	}
}
