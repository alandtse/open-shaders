#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

/** @brief Read-only compatibility adapter for legacy feature overwrite files. */
class SettingsOverrideManager
{
public:
	struct OverrideInfo
	{
		std::string modName;
		std::string featureName;
		std::string filePath;
		json overrideData;
		bool isGlobal = false;
		std::string version;
		std::string description;
		bool enabled = true;
	};

	struct LegacyPack
	{
		std::string id;
		std::string name;
		std::string version;
		std::string description;
		std::filesystem::path sourcePath;
		std::map<std::string, json> documents;
		std::map<std::string, std::filesystem::path> documentSources;
		std::unordered_map<std::string, bool> disableAtBoot;
	};

	/** @brief Returns the read-only legacy adapter. */
	static SettingsOverrideManager* GetSingleton()
	{
		static SettingsOverrideManager instance;
		return &instance;
	}

	/** @brief Reads legacy files without applying a separate settings layer. */
	size_t DiscoverOverrides();
	/** @brief Projects recognized legacy feature values into standard Baseline documents. */
	const std::vector<LegacyPack>& GetBaselinePacks() const;
	/** @brief Imports companion personal edits until the main settings file has saved their migration. */
	void ImportUserSettings(json& settings) const;
	/** @brief Captures underlying settings for reload rollback. */
	void CaptureBaseSettings(const json& settings) { userBaseline = settings; }
	/** @brief Captures normalized values for detecting subsequent live edits. */
	void CaptureAppliedSettings(const json& settings) { appliedSettings = settings; }
	/** @brief Returns captured underlying settings. */
	const json& GetBaseSettings() const { return userBaseline; }
	/** @brief Returns captured applied settings. */
	const json& GetAppliedSettings() const { return appliedSettings; }
	/** @brief Returns live edits relative to captured settings. */
	json GetPendingEdits(const json& settings) const;

private:
	std::filesystem::path GetOverridesDirectory() const;
	std::unique_ptr<OverrideInfo> LoadOverrideFile(const std::filesystem::path& filePath);
	std::pair<std::string, std::string> ParseOverrideFilename(const std::string& filename);
	bool ValidateOverrideFormat(const json& overrideJson, const std::string& filePath);
	bool ValidateJsonDataTypes(const json& data, const std::string& path, const std::string& filePath, size_t depth = 0);
	json SanitizeJsonData(const json& data);
	void ReportOverrideFailure(const std::string& modName, const std::string& featureName, const std::string& message);

	std::vector<OverrideInfo> overrides;
	mutable std::optional<std::vector<LegacyPack>> translatedPacks;
	json userBaseline;
	json appliedSettings;
	static constexpr size_t MAX_JSON_DEPTH = 10;
	static constexpr size_t MAX_STRING_LENGTH = 1000;
	static constexpr size_t MAX_ARRAY_SIZE = 100;
	static constexpr size_t MAX_OBJECT_SIZE = 100;
	static constexpr double MAX_NUMERIC_VALUE = 1e6;
	static constexpr double MIN_NUMERIC_VALUE = -1e6;
};
