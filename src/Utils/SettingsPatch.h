#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

struct Feature;

namespace Util::Settings
{
	/** @brief Installed value and companion customization winning at one overwrite-owned setting. */
	struct OverwriteValue
	{
		const json* installed = nullptr;
		const json* user = nullptr;
	};
	/** @brief Feature overwrites outrank global companion edits; companions require an installed owner. */
	OverwriteValue ResolveOverwriteValue(const json* global, const json* globalUser, const json* feature, const json* featureUser);

	/**
	 * @brief Overlays recognized roots, validating known values and preserving owner-defined nested data.
	 * @param a_defaults Current default settings and destination.
	 * @param a_userSettings Untrusted partial user settings.
	 */
	void OverlayRecognizedRootSettings(json& a_defaults, const json& a_userSettings);
	/** @brief Accepts a safe uint32 input binding or flat list, retaining the default on malformed input. */
	void OverlayInputBinding(json& a_binding, const json& a_userBinding);

	/**
	 * @brief Builds the minimal user layer for values controlled by an override.
	 * @param a_current Current effective settings.
	 * @param a_override Mod-provided override settings.
	 * @return Changed values shaped like a_override; empty when they match.
	 */
	json BuildUserOverride(const json& a_current, const json& a_override);
	/** @brief Retains explicitly saved keys even when their values equal the current baseline. */
	json BuildUserOverride(const json& current, const json& baseline, const json& savedKeys);
	/** @brief Removes top-level metadata from a settings object. */
	void EraseMetadataKeys(json& settings);
	/** @brief Merges objects recursively, replacing values whose shape changes. */
	void MergeSettings(json& target, const json& overlay);
	/** @brief Validates a known setting's JSON type and numeric range. */
	bool IsCompatibleSettingValue(const json& known, const json& value);
	/** @brief Removes known settings with incompatible types while preserving unknown keys. */
	void EraseMismatchedSettings(json& incoming, const json& known, const std::string& prefix, std::vector<std::string>& erasedKeys);

	/** @brief Selects values present in both a_values and a_mask, retaining their nested structure. */
	json SelectSettings(const json& a_values, const json& a_mask);
	/** @brief Replaces masked values from a_source, removing masked keys absent from that source. */
	void RestoreSettings(json& a_target, const json& a_source, const json& a_mask);
	/** @brief Selects complete JSON leaves by pointer, preserving object keys and indivisible arrays. */
	json SelectSettingPaths(const json& values, const std::vector<std::string>& paths);
	/** @brief Erases one setting leaf and empty parent groups without removing metadata or array elements. */
	bool EraseSettingPath(json& document, const json::json_pointer& pointer);

	// Collects keys of a_incoming that a_known has no counterpart for, as dotted
	// paths, recursing into nested groups. The settings serializers drop keys they
	// don't recognize, so a mis-nested or misspelled key would otherwise apply
	// nothing while still reporting success.
	void CollectUnknownSettingKeys(const json& a_incoming, const json& a_known,
		const std::string& a_prefix, std::vector<std::string>& a_out);

	/**
	 * @brief Merges a partial settings blob onto a feature's live settings and applies it.
	 * @param a_feature Feature whose settings are being patched.
	 * @param a_patch Partial settings object; unrecognized keys are rejected (see o_unknownKeys)
	 *        rather than silently dropped.
	 * @param o_unknownKeys Filled with dotted-path keys a_patch names that the feature's
	 *        SaveSettings blob doesn't, when this returns false.
	 * @return False (nothing applied) if a_patch names any unrecognized key; true otherwise.
	 * @note Main/render thread only -- calls the feature's SaveSettings/LoadSettings directly.
	 */
	bool ApplyPatch(Feature& a_feature, const json& a_patch, std::vector<std::string>& o_unknownKeys);
}
