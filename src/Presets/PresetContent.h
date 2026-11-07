#pragma once

#include "PresetActivation.h"
#include "Utils/SettingsPatch.h"

#include <cmath>
#include <map>

namespace PresetContent
{
	using Selection = std::map<std::filesystem::path, std::vector<std::string>>;

	enum class FileFilter
	{
		All,
		Baseline,
		Scene,
		Records
	};

	enum class SettingEditResult
	{
		Changed,
		Unchanged,
		Missing,
		Stale,
		InvalidValue,
		Blocked,
		Conflict,
		Failed
	};

	struct File
	{
		std::filesystem::path relative;
		std::filesystem::path source;
		std::optional<nlohmann::json> document;
	};

	struct SettingEdit
	{
		std::string packId;
		std::filesystem::path relative;
		std::string path;
		nlohmann::json expected;
		nlohmann::json value;
	};

	/** @brief Lists primary payload files without requiring the preset to be enabled. */
	std::vector<File> List(const std::string& packId);
	/** @brief Reads one value using its registered numeric type when available. */
	nlohmann::json EditableValue(const File& file, const std::string& path);
	/** @brief Replaces an unchanged value using declared types and preserving unrelated data. */
	inline SettingEditResult ReplaceSetting(nlohmann::json& document, const std::string& path,
		const nlohmann::json& expected, const nlohmann::json& value, const nlohmann::json* prototype = nullptr)
	try {
		using json = nlohmann::json;
		const json::json_pointer pointer(path);
		if (pointer.empty() || path.find("/_") != std::string::npos || !document.contains(pointer))
			return SettingEditResult::Missing;
		auto& current = document.at(pointer);
		if (current != expected || current.type() != expected.type())
			return SettingEditResult::Stale;
		if (current == value)
			return SettingEditResult::Unchanged;
		json normalized = value;
		const auto normalizeValue = [](auto&& self, const json& original, json& edited) -> bool {
			if (edited.is_discarded() || (edited.is_number_float() && !std::isfinite(edited.get<double>())))
				return false;
			if (!Util::Settings::IsCompatibleSettingValue(original, edited))
				return false;
			if (original.is_number()) {
				if (edited.is_boolean())
					edited = edited.get<bool>() ? 1 : 0;
				if (original.is_number_float())
					edited = edited.get<double>();
				else if (original.is_number_unsigned())
					edited = edited.get<std::uint64_t>();
				else
					edited = edited.get<std::int64_t>();
			} else if (original.is_array() && !original.empty()) {
				for (size_t index = 0; index < edited.size(); ++index)
					if (!self(self, original[original.size() == edited.size() ? index : 0], edited[index]))
						return false;
			} else if (original.is_object()) {
				if (original.size() != edited.size())
					return false;
				for (const auto& [key, member] : original.items())
					if (!edited.contains(key) || !self(self, member, edited[key]))
						return false;
			} else if (edited.is_structured()) {
				for (auto& member : edited)
					if (!self(self, nullptr, member))
						return false;
			}
			return true;
		};
		if (!normalizeValue(normalizeValue, prototype ? *prototype : current, normalized))
			return SettingEditResult::InvalidValue;
		current = std::move(normalized);
		return SettingEditResult::Changed;
	} catch (const nlohmann::json::exception&) {
		return SettingEditResult::InvalidValue;
	}
	/** @brief Whether a relative file supplies one feature's Base Settings. */
	inline bool IsBaselineFile(const std::filesystem::path& relative)
	{
		return relative.parent_path() == "Baseline" && relative.extension() == ".json";
	}
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
	/** @brief Whether a relative file identifies one OS Editor record document. */
	inline bool IsRecordFile(const std::filesystem::path& relative)
	{
		return !relative.empty() && !relative.has_root_path() && relative.extension() == ".json" &&
		       std::distance(relative.begin(), relative.end()) == 3 && *relative.begin() == "Forms" &&
		       std::ranges::none_of(relative, [](const auto& part) { return part == "." || part == ".."; });
	}
	/** @brief Selects JSON payloads by the file groups exposed in Manage Preset. */
	inline bool MatchesFileFilter(const std::filesystem::path& relative, FileFilter filter)
	{
		if (relative.empty() || relative.has_root_path() || std::ranges::any_of(relative, [](const auto& part) { return part == ".."; }) || relative.extension() != ".json")
			return false;
		const bool baseline = IsBaselineFile(relative);
		const bool scene = IsSceneFile(relative);
		const bool record = IsRecordFile(relative);
		switch (filter) {
		case FileFilter::Baseline:
			return baseline;
		case FileFilter::Scene:
			return scene;
		case FileFilter::Records:
			return record;
		default:
			return baseline || scene || record;
		}
	}
	/** @brief Reads a scene payload's declared feature or its legacy filename suffix. */
	inline std::string SceneFeatureName(const std::filesystem::path& relative, const nlohmann::json& document)
	{
		if (const auto feature = document.find("_feature"); feature != document.end()) {
			if (!feature->is_string())
				return {};
			if (!feature->get_ref<const std::string&>().empty())
				return feature->get<std::string>();
		}
		const auto stem = Util::PathToUtf8(relative.stem());
		const auto split = stem.rfind('_');
		return split == std::string::npos ? std::string{} : stem.substr(split + 1);
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
			const auto featureName = SceneFeatureName(relative, document);
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
