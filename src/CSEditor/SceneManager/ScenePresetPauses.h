#pragma once

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ScenePresetPauses
{
	/// File, feature, setting path, setting name and period identify one authored entry.
	using Key = std::tuple<std::string, std::string, std::vector<std::string>, std::string, std::string>;
	using State = std::set<Key>;
	using Changes = std::map<Key, bool>;
	inline constexpr auto kDocumentKey = "presetPauses";

	inline void Set(State& state, const Key& key, bool paused)
	{
		if (paused)
			state.insert(key);
		else
			state.erase(key);
	}

	inline void Preview(Changes& changes, const State& base, const Key& key, bool paused)
	{
		if (base.contains(key) == paused)
			changes.erase(key);
		else
			changes.insert_or_assign(key, paused);
	}

	inline void Apply(State& state, const Changes& changes)
	{
		for (const auto& [key, paused] : changes)
			Set(state, key, paused);
	}

	inline State Read(const nlohmann::json& document)
	{
		State state;
		const auto rows = document.find(kDocumentKey);
		if (rows == document.end() || !rows->is_array())
			return state;
		for (const auto& row : *rows) {
			if (!row.is_object())
				continue;
			const auto file = row.find("file");
			const auto feature = row.find("feature");
			const auto path = row.find("path");
			const auto setting = row.find("setting");
			const auto period = row.find("period");
			if (file == row.end() || !file->is_string() || feature == row.end() || !feature->is_string() ||
				path == row.end() || !path->is_array() || setting == row.end() || !setting->is_string() ||
				period == row.end() || !period->is_string())
				continue;
			std::vector<std::string> parts;
			for (const auto& part : *path)
				if (part.is_string())
					parts.push_back(part.get<std::string>());
			if (parts.size() == path->size())
				state.emplace(file->get<std::string>(), feature->get<std::string>(), std::move(parts),
					setting->get<std::string>(), period->get<std::string>());
		}
		return state;
	}

	inline void Write(nlohmann::json& document, const State& state)
	{
		if (state.empty()) {
			document.erase(kDocumentKey);
			return;
		}
		auto rows = nlohmann::json::array();
		for (const auto& [file, feature, path, setting, period] : state)
			rows.push_back({ { "file", file }, { "feature", feature }, { "path", path }, { "setting", setting }, { "period", period } });
		document[kDocumentKey] = std::move(rows);
	}
}
