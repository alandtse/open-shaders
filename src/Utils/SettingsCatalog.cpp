#include "SettingsCatalog.h"

#include "I18n/I18n.h"
#include "Utils/Format.h"
#include "Utils/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <ranges>
#include <set>

namespace Util::Settings
{
	constexpr std::string_view kImGuiIdSeparator = "##";
	constexpr std::string_view kLabelSeparator = " / ";
	/// Decimals FormatValue shows for a float, before trailing zeros are trimmed.
	constexpr int kValueDecimals = 3;

	/// Joins the non-empty parts into the "Group / Setting" form export lists show.
	static std::string JoinLabelParts(const std::vector<std::string>& parts)
	{
		return parts | std::views::filter([](const auto& part) { return !part.empty(); }) |
		       std::views::join_with(kLabelSeparator) | std::ranges::to<std::string>();
	}

	std::string StripImGuiId(std::string_view label)
	{
		return std::string(label.substr(0, label.find(kImGuiIdSeparator)));
	}

	std::vector<std::string> SplitCatalogPath(std::string_view path)
	{
		std::vector<std::string> parts;
		size_t start = 0;
		while (start < path.size()) {
			auto end = path.find('/', start);
			auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
			if (!part.empty()) {
				std::string decoded(part);
				for (size_t pos = 0; (pos = decoded.find('~', pos)) != std::string::npos;) {
					if (pos + 1 < decoded.size() && decoded[pos + 1] == '1')
						decoded.replace(pos, 2, "/");
					else if (pos + 1 < decoded.size() && decoded[pos + 1] == '0')
						decoded.replace(pos, 2, "~");
					++pos;
				}
				parts.push_back(std::move(decoded));
			}
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return parts;
	}

	bool IsStructuralDisplayPart(std::string_view part)
	{
		std::string normalized;
		normalized.reserve(part.size());
		for (const char ch : part)
			if (std::isalnum(static_cast<unsigned char>(ch)))
				normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
		return normalized == "settings" || normalized == "values" || normalized == "baseline";
	}

	std::string NormalizeDisplayPart(std::string part)
	{
		part = StripImGuiId(part);
		if (!part.empty() && std::all_of(part.begin(), part.end(), [](const char ch) {
				return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
			}))
			part = Util::PrettifyIdentifier(part);
		return part;
	}

	std::vector<std::string> GetCatalogDisplayPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
		const auto keys = SplitCatalogPath(setting.displayPathKeys);
		for (size_t index = 0; index < parts.size(); ++index) {
			if (index < keys.size() && keys[index] != "-")
				parts[index] = T(keys[index], parts[index].c_str());
			parts[index] = NormalizeDisplayPart(std::move(parts[index]));
		}
		std::erase_if(parts, [](const auto& part) { return part.empty() || IsStructuralDisplayPart(part); });
		return parts;
	}

	std::string GetCatalogLeafDisplayName(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		if (setting.displayName.empty() && setting.displayNameKey.empty() &&
			setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Choice)
			return T("feature.scene_manager.selection", "Selection");

		auto displayName = StripImGuiId(setting.displayName.empty() ? setting.settingKey : setting.displayName);
		if (!setting.displayNameKey.empty())
			displayName = StripImGuiId(T(setting.displayNameKey, displayName.c_str()));
		return displayName;
	}

	std::vector<ExportSetting> GetExportSettings(std::string_view featureShortName, const nlohmann::json& settings)
	{
		using json = nlohmann::json;
		std::map<std::string, std::string> labels;
		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (setting.featureShortName != featureShortName ||
				!SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Persisted))
				continue;
			json::json_pointer path;
			for (const auto& part : SplitCatalogPath(setting.serializedPath))
				path /= part;
			path /= std::string(setting.serializedKey);
			auto parts = GetCatalogDisplayPath(setting);
			parts.push_back(GetCatalogLeafDisplayName(setting));
			labels.try_emplace(path.to_string(), JoinLabelParts(parts));
		}
		std::vector<ExportSetting> result;
		const auto visit = [&](auto&& self, const json& node, const json::json_pointer& parent, const std::string& context) -> void {
			for (const auto& [key, value] : node.items()) {
				if (IsMetadataKey(key))
					continue;
				auto path = parent / key;
				auto label = JoinLabelParts({ context, NormalizeDisplayPart(key) });
				if (value.is_object()) {
					self(self, value, path, label);
				} else {
					if (const auto found = labels.find(path.to_string()); found != labels.end() && !found->second.empty())
						label = found->second;
					result.push_back({ path.to_string(), std::move(label) });
				}
			}
		};
		if (settings.is_object())
			visit(visit, settings, json::json_pointer{}, {});
		std::ranges::sort(result, [](const auto& a, const auto& b) {
			return a.label == b.label ? a.path < b.path : a.label < b.label;
		});
		return result;
	}

	std::string FormatValue(const nlohmann::json& value)
	{
		if (value.is_string())
			return value.get<std::string>();
		if (value.is_boolean())
			return value.get<bool>() ? T("cs_editor.scene_copy_value_on", "On") : T("cs_editor.scene_copy_value_off", "Off");
		if (value.is_array()) {
			std::string text = "[";
			for (const auto& element : value)
				text += (text.size() > 1 ? ", " : "") + FormatValue(element);
			return text + "]";
		}
		if (!value.is_number_float())
			return value.dump();
		auto text = std::format("{:.{}f}", value.get<double>(), kValueDecimals);
		text.erase(text.find_last_not_of('0') + 1);
		if (text.ends_with('.'))
			text.pop_back();
		return text;
	}

	std::vector<std::string> GetCatalogSelectorPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = SplitCatalogPath(setting.selectorPath);
		auto keys = SplitCatalogPath(setting.selectorPathKeys);
		for (size_t i = 0; i < parts.size(); ++i) {
			if (i < keys.size() && keys[i] != "-")
				parts[i] = T(keys[i], parts[i].c_str());
			parts[i] = StripImGuiId(parts[i]);
		}
		return parts;
	}

	std::vector<std::string> GetCatalogContextPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = GetCatalogDisplayPath(setting);
		auto selectorDefaults = GetCatalogSelectorPath(setting);
		auto rawParts = SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
		const auto rawKeys = SplitCatalogPath(setting.displayPathKeys);
		const auto settingParts = SplitCatalogPath(setting.settingPath);
		const bool hasSelector = !selectorDefaults.empty();
		size_t rawOffset = 0;
		for (auto& part : selectorDefaults)
			part = NormalizeDisplayPart(std::move(part));
		while (!parts.empty() && !selectorDefaults.empty() &&
			   Util::IEquals(parts.front(), selectorDefaults.front())) {
			parts.erase(parts.begin());
			selectorDefaults.erase(selectorDefaults.begin());
			++rawOffset;
		}
		if (hasSelector) {
			while (rawOffset < rawParts.size() && IsStructuralDisplayPart(rawParts[rawOffset]))
				++rawOffset;
			if (!parts.empty() && rawOffset < rawParts.size() && rawOffset < settingParts.size()) {
				const bool translated = rawOffset < rawKeys.size() && rawKeys[rawOffset] != "-";
				auto rawPart = NormalizeDisplayPart(rawParts[rawOffset]);
				auto settingPart = NormalizeDisplayPart(settingParts[rawOffset]);
				if (!translated && Util::IEquals(parts.front(), rawPart) &&
					Util::IEquals(rawPart, settingPart))
					parts.erase(parts.begin());
			}
		}
		return parts;
	}

	namespace
	{
		/** @brief Parses a whole segment as an array index. */
		bool ParseCatalogArrayIndex(std::string_view value, size_t& index)
		{
			const auto result = std::from_chars(value.data(), value.data() + value.size(), index);
			return result.ec == std::errc{} && result.ptr == value.data() + value.size();
		}

		/** @brief The node at a catalog path, stepping through objects by key and arrays by index. */
		template <class Json>
		Json* GetCatalogNodeAtPath(Json& data, const std::vector<std::string>& path)
		{
			auto* node = &data;
			for (const auto& segment : path) {
				if (node->is_object()) {
					auto it = node->find(segment);
					if (it == node->end())
						return nullptr;
					node = &*it;
					continue;
				}

				size_t index = 0;
				if (!node->is_array() || !ParseCatalogArrayIndex(segment, index) || index >= node->size())
					return nullptr;
				node = &(*node)[index];
			}
			return node;
		}

		/** @brief The node a setting serializes to, narrowed to its component; null when absent. */
		template <class Json>
		Json* GetCatalogSerializedValueImpl(Json& data, const SceneSettingsCatalog::SettingMetadata& setting)
		{
			auto* parent = GetCatalogNodeAtPath(data, SplitCatalogPath(setting.serializedPath));
			if (!parent)
				return nullptr;

			Json* value = nullptr;
			if (parent->is_object()) {
				auto valueIt = parent->find(setting.serializedKey);
				if (valueIt == parent->end())
					return nullptr;
				value = &*valueIt;
			} else {
				size_t index = 0;
				if (!parent->is_array() || !ParseCatalogArrayIndex(setting.serializedKey, index) ||
					index >= parent->size())
					return nullptr;
				value = &(*parent)[index];
			}

			if (setting.serializedComponent < 0)
				return value;
			const auto component = static_cast<size_t>(setting.serializedComponent);
			if (!value->is_array() || component >= value->size())
				return nullptr;
			return &(*value)[component];
		}
	}

	nlohmann::json* GetCatalogSerializedValue(nlohmann::json& data, const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return GetCatalogSerializedValueImpl(data, setting);
	}

	const nlohmann::json* GetCatalogSerializedValue(const nlohmann::json& data, const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return GetCatalogSerializedValueImpl(data, setting);
	}
}
