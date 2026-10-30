#pragma once

#include "PresetActivation.h"
#include "Utils/SettingsPatch.h"

namespace PresetContent
{
	struct File
	{
		std::filesystem::path relative;
		std::filesystem::path source;
		std::optional<nlohmann::json> document;
	};

	/** @brief Lists primary payload files without requiring the preset to be enabled. */
	std::vector<File> List(const std::string& packId);
	/** @brief Removes selected JSON paths and their per-setting scene metadata. */
	inline bool RemoveSettings(nlohmann::json& document, std::span<const std::string> paths)
	{
		bool changed = false;
		for (const auto& value : paths) {
			const nlohmann::json::json_pointer path(value);
			if (path.empty() || value.starts_with("/_"))
				continue;
			changed |= Util::Settings::EraseSettingPath(document, path);
			if (auto metadata = document.find("_metadata"); metadata != document.end() && metadata->is_object())
				for (const auto key : { "entryTransitions", "entryDeletions" })
					if (auto entries = metadata->find(key); entries != metadata->end() && entries->is_object())
						Util::Settings::EraseSettingPath(*entries, path);
		}
		return changed;
	}

	/** @brief Normalizes payload addresses without narrowing Unicode filenames. */
	inline std::string PathKey(const std::filesystem::path& path)
	{
		auto key = Util::PathToUtf8(path);
		std::ranges::replace(key, '\\', '/');
		return Util::ToLowerAscii(key);
	}

	/** @brief Whether a relative file belongs to a native scene context. */
	inline bool IsSceneFile(const std::filesystem::path& relative)
	{
		if (relative.empty() || relative.has_root_path() || std::ranges::any_of(relative, [](const auto& part) { return part == ".."; }) || relative.extension() != ".json")
			return false;
		const auto first = Util::PathToUtf8(*relative.begin());
		return first == "InteriorOnly" || first == "TimeOfDay" || first == "Weather" || first == "Locations";
	}

	/** @brief Flattens setting objects while keeping arrays and scalar values indivisible. */
	inline void CollectValues(const nlohmann::json& values, const std::string& prefix, PresetActivation::Footprint& result)
	{
		if (!values.is_object()) {
			result[prefix] = values;
			return;
		}
		for (const auto& [key, value] : values.items())
			if (!key.starts_with('_')) {
				nlohmann::json::json_pointer pointer;
				pointer /= key;
				CollectValues(value, prefix + pointer.to_string(), result);
			}
	}

	/** @brief Returns the exact setting and record addresses a native payload supplies. */
	inline PresetActivation::Footprint Describe(const std::filesystem::path& relative, const nlohmann::json& document)
	{
		PresetActivation::Footprint result;
		if (!document.is_object() || relative.empty() || relative.has_root_path())
			return result;
		const auto first = Util::PathToUtf8(*relative.begin());
		if (first == "Baseline")
			CollectValues(document, "base/" + Util::PathToUtf8(relative.stem()), result);
		else if (first == "Forms")
			result["record/" + PathKey(relative)] = document;
		else if (IsSceneFile(relative)) {
			const auto feature = document.find("_feature");
			if (feature != document.end() && !feature->is_string())
				return result;
			auto featureName = feature == document.end() ? std::string{} : feature->get<std::string>();
			if (featureName.empty()) {
				const auto stem = Util::PathToUtf8(relative.stem());
				if (const auto split = stem.rfind('_'); split != std::string::npos)
					featureName = stem.substr(split + 1);
			}
			if (featureName.empty())
				return result;
			const auto context = PathKey(relative.parent_path());
			const auto metadata = document.find("_metadata");
			PresetActivation::Footprint values;
			CollectValues(document, "", values);
			for (const auto& [path, value] : values) {
				nlohmann::json state{ { "value", value }, { "deleted", false }, { "transition", nullptr } };
				if (metadata != document.end() && metadata->is_object()) {
					for (const auto& [key, field] : { std::pair{ "entryDeletions", "deleted" }, std::pair{ "entryTransitions", "transition" } })
						if (const auto entries = metadata->find(key); entries != metadata->end() && entries->is_object()) {
							const nlohmann::json::json_pointer pointer(path);
							if (entries->contains(pointer))
								state[field] = entries->at(pointer);
						}
				}
				result["scene/" + context + "/" + featureName + path] = std::move(state);
			}
			if (metadata != document.end() && metadata->is_object()) {
				auto owner = relative.parent_path();
				if ((first == "Weather" || first == "Locations") && std::distance(owner.begin(), owner.end()) > 2)
					owner = owner.parent_path();
				for (const auto key : { "timeOfDayEnabled", "targetType" })
					if (metadata->contains(key))
						result["context/" + PathKey(owner) + "/" + key] = metadata->at(key);
			}
		}
		return result;
	}
}
