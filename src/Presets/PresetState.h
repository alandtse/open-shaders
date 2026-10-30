#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Utils/StringUtils.h"

namespace PresetState
{
	inline constexpr std::uintmax_t kMaxDocumentBytes = 1024 * 1024;
	inline constexpr const char* kFeatureSettingsVersionKey = "featureSettingsVersion";
	inline constexpr int kFeatureSettingsVersion = 1;
	inline constexpr const char* kNormalizedFeatureNamesKey = "normalizedFeatureNames";

	/** @brief Whether this feature's fields are normalized edits instead of unmigrated legacy settings. */
	inline bool HasNormalizedFeatureEdits(const nlohmann::json& state, std::string_view featureName)
	{
		if (!state.is_object())
			return false;
		const auto version = state.find(kFeatureSettingsVersionKey);
		const auto names = state.find(kNormalizedFeatureNamesKey);
		return version != state.end() && version->is_number_integer() && *version == kFeatureSettingsVersion &&
		       names != state.end() && names->is_array() && std::ranges::any_of(*names, [&](const nlohmann::json& name) {
				   return name.is_string() && name.get_ref<const std::string&>() == featureName;
			   });
	}

	/** @brief Records a serialized feature without marking preserved inactive legacy roots as normalized. */
	inline void MarkNormalizedFeatureEdits(nlohmann::json& state, const std::string& featureName)
	{
		if (!state.is_object())
			state = nlohmann::json::object();
		const auto version = state.find(kFeatureSettingsVersionKey);
		const bool currentVersion = version != state.end() && version->is_number_integer() && *version == kFeatureSettingsVersion;
		auto& names = state[kNormalizedFeatureNamesKey];
		if (!currentVersion || !names.is_array())
			names = nlohmann::json::array();
		if (std::ranges::find(names, nlohmann::json(featureName)) == names.end())
			names.push_back(featureName);
		state[kFeatureSettingsVersionKey] = kFeatureSettingsVersion;
	}

	/** @brief Resolves the first existing payload without falling through malformed primary files. */
	inline std::filesystem::path FindFirstExistingPayload(std::span<const std::filesystem::path> roots, const std::filesystem::path& relative)
	{
		const auto normalized = relative.lexically_normal();
		if (normalized.empty() || normalized.has_root_path() || *normalized.begin() == "..")
			return {};
		for (const auto& root : roots) {
			const auto path = root / normalized;
			std::error_code error;
			if (std::filesystem::exists(path, error) || error)
				return path;
		}
		return {};
	}

	/** @brief Whether an identifier names one visible preset folder. */
	inline bool IsPackId(std::string_view id)
	{
		if (!Util::IsSinglePathComponent(id) || id.front() == '_' || id.front() == '.' ||
			id.back() == '.' || id.back() == ' ' || id.find_first_of("*?\"<>|") != std::string_view::npos ||
			std::ranges::any_of(id, [](unsigned char value) { return value < 32 || value == 127; }))
			return false;
		const auto stem = Util::ToLowerAscii(id.substr(0, id.find('.')));
		if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$")
			return false;
		return !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem.back() >= '1' && stem.back() <= '9');
	}

	/** @brief Accepts a visible pack name or an opaque provider selection without path traversal. */
	inline bool IsCatalogId(std::string_view id)
	{
		if (id.empty() || IsPackId(id))
			return true;
		const auto separator = id.find(':');
		return separator != std::string_view::npos && separator > 0 && separator + 1 < id.size() &&
		       id.find(':', separator + 1) == std::string_view::npos &&
		       IsPackId(id.substr(0, separator)) && IsPackId(id.substr(separator + 1));
	}

	/** @brief Preserves unknown state while normalizing the active pack and ordered Baselines. */
	inline nlohmann::json Normalize(const nlohmann::json& state, bool (*validActiveId)(std::string_view) = IsCatalogId)
	{
		auto result = state.is_object() ? state : nlohmann::json::object();
		const auto active = result.find("activePackId");
		if (active == result.end() || !active->is_string() || !validActiveId(active->get_ref<const std::string&>()))
			result["activePackId"] = "";
		std::vector<std::string> ids;
		if (const auto baselines = result.find("baselinePackIds"); baselines != result.end() && baselines->is_array())
			for (const auto& id : *baselines)
				if (id.is_string() && IsPackId(id.get_ref<const std::string&>()) &&
					std::ranges::find(ids, id.get_ref<const std::string&>()) == ids.end())
					ids.push_back(id.get<std::string>());
		result["baselinePackIds"] = std::move(ids);
		return result;
	}

	/** @brief Blends linear progress toward the upstream smootherstep transition. */
	inline float Ease(float progress, float weight)
	{
		progress = std::clamp(progress, 0.0f, 1.0f);
		const auto smooth = progress * progress * progress * (progress * (progress * 6.0f - 15.0f) + 10.0f);
		return std::lerp(progress, smooth, std::clamp(weight, 0.0f, 1.0f));
	}

	struct BlendedPeriods
	{
		std::array<float, 6> starts;
		float blendHours;
	};

	/** @brief Fits transition windows inside their periods and applies the upstream boundary shifts. */
	inline BlendedPeriods BlendPeriodBoundaries(std::array<float, 6> boundaries, float blendHours)
	{
		constexpr std::array offsets{ 0.5f, 0.0f, -0.5f, 0.5f, 0.0f, -0.5f };
		for (size_t index = 0; index < boundaries.size(); ++index) {
			const auto next = (index + 1) % boundaries.size();
			const float end = next ? boundaries[next] : boundaries.front() + 24.0f;
			const float needed = 1.0f + offsets[index] - offsets[next];
			if (needed > 0.0f)
				blendHours = std::min(blendHours, (end - boundaries[index]) / needed);
		}
		for (size_t index = 0; index < boundaries.size(); ++index) boundaries[index] += offsets[index] * blendHours;
		return { boundaries, blendHours };
	}

	/** @brief Computes six period weights from climate boundaries, blend width and game hour. */
	inline std::array<float, 6> BlendPeriods(std::array<float, 6> boundaries, float blendHours, float ease, float hour)
	{
		const auto blended = BlendPeriodBoundaries(boundaries, blendHours);
		boundaries = blended.starts;
		blendHours = blended.blendHours;
		hour -= blendHours * 0.5f;
		if (hour < 0.0f)
			hour += 24.0f;
		std::array<float, 6> factors{};
		for (size_t index = 0; index < boundaries.size(); ++index) {
			const auto next = (index + 1) % boundaries.size();
			const float end = next ? boundaries[next] : boundaries.front() + 24.0f;
			const float current = end > 24.0f && hour < boundaries[index] ? hour + 24.0f : hour;
			if (current < boundaries[index] || current >= end)
				continue;
			const float toEnd = end - current;
			factors[index] = toEnd >= blendHours ? 1.0f : Ease(toEnd / blendHours, ease);
			factors[next] = 1.0f - factors[index];
			return factors;
		}
		factors[2] = 1.0f;
		return factors;
	}
}
