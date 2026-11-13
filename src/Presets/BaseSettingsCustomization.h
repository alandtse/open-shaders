#pragma once

#include "Utils/SettingsPatch.h"

namespace BaseSettingsCustomization
{
	/** @brief Records installed suppliers at object leaves, treating arrays as complete settings. */
	inline void AddSource(json& sources, const std::string& provider, const json& values, const json::json_pointer& path = json::json_pointer{})
	{
		if (values.is_object()) {
			for (const auto& [key, value] : values.items())
				if (!key.starts_with('_'))
					AddSource(sources, provider, value, path / key);
		} else if (!path.empty())
			sources[path.to_string()][provider] = values;
	}

	struct Reconciled
	{
		json state;
		json removed = json::object();
	};

	/** @brief Invalidates changed suppliers and settings without any remaining enabled owner. */
	inline Reconciled Reconcile(const json& previous, const json& sources, const json& active)
	{
		Reconciled result{ { { "sources", sources }, { "active", active }, { "edits", json::object() } } };
		const auto oldSources = previous.value("sources", json::object());
		const auto oldActive = previous.value("active", json::object());
		const auto oldEdits = previous.value("edits", json::object());
		const auto remove = [&](const std::string& path) {
			const json::json_pointer pointer(path);
			if (oldActive.contains(pointer) || oldEdits.contains(pointer))
				result.removed[pointer] = true;
		};
		for (const auto& [path, providers] : oldSources.items())
			if (!sources.contains(path) || sources.at(path) != providers)
				remove(path);
		for (const auto& [path, providers] : sources.items())
			if (!oldSources.contains(path) && previous.contains("sources"))
				remove(path);
		json owned = json::object();
		AddSource(owned, "active", oldActive);
		for (const auto& [path, _] : owned.items())
			if (!active.contains(json::json_pointer(path)))
				remove(path);
		owned.clear();
		AddSource(owned, "active", active);
		for (const auto& [path, _] : owned.items()) {
			const json::json_pointer pointer(path);
			if (!oldActive.contains(pointer))
				result.removed[pointer] = true;
		}
		auto edits = Util::Settings::SelectSettings(oldEdits, active);
		Util::Settings::RestoreSettings(edits, json::object(), result.removed);
		result.state["edits"] = std::move(edits);
		return result;
	}

	/** @brief Separates ordinary saved values from explicit customizations above enabled presets. */
	inline json UserLayer(json settings, const json& state)
	{
		Util::Settings::RestoreSettings(settings, json::object(), state.value("active", json::object()));
		Util::Settings::MergeSettings(settings, state.value("edits", json::object()));
		return settings;
	}

	/** @brief Captures only post-application edits, removing customizations reset to their preset value. */
	inline json SaveEdits(json state, const json& current, const json& applied)
	{
		const auto active = state.value("active", json::object());
		auto edits = Util::Settings::SelectSettings(state.value("edits", json::object()), active);
		const auto pending = Util::Settings::SelectSettings(Util::Settings::BuildUserOverride(current, applied), active);
		Util::Settings::RestoreSettings(edits, Util::Settings::BuildUserOverride(current, active), pending);
		state["edits"] = std::move(edits);
		return state;
	}
}
