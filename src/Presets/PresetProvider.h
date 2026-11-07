#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** @brief Optional feature-owned adapters for portable preset payloads and authoring. */
class PresetProvider
{
public:
	struct LibraryEntry
	{
		std::string id;
		std::string name;
		std::filesystem::path rootPath;
		bool valid = true;
		std::string invalidReason;
		bool legacy = false;
	};

	virtual ~PresetProvider() = default;
	/** @brief Returns the provider's stable manifest backend key, if it supplies a payload. */
	virtual std::string_view GetBackendId() const { return {}; }
	/** @brief Returns the upstream manifest type associated with this payload. */
	virtual std::string_view GetManifestType() const { return {}; }
	/** @brief Prepares legacy imports before catalog discovery. */
	virtual void PrepareDiscovery() {}
	/** @brief Resolves a valid payload beneath a pack, or an empty path when absent. */
	virtual std::filesystem::path ResolvePayloadRoot(const std::filesystem::path&, const nlohmann::json&) const { return {}; }
	/** @brief Discovers standalone or legacy payloads outside the portable pack library. */
	virtual std::vector<LibraryEntry> ScanLibrary() const { return {}; }
	/** @brief Returns an already-running legacy payload's catalog identifier when no selection was saved. */
	virtual std::string GetDefaultActivePackId() const { return {}; }
	/** @brief Maps a catalog pack identifier to the feature's existing payload identifier. */
	virtual std::string GetPresetId(const std::string& packId) const { return packId; }
	/** @brief Returns the selected payload identifier, preserving an empty legacy selection. */
	virtual std::optional<std::string> GetActivePresetId() const { return std::nullopt; }
	/** @brief Applies a discovered payload, optionally saving its current values first. */
	virtual bool Apply(const std::string&, bool) { return false; }
	/** @brief Returns whether the feature's current payload can be exported. */
	virtual bool CanExport() const { return false; }
	/** @brief Returns whether this provider owns the current output pipeline. */
	virtual bool IsPipelineActive() const { return false; }
	/** @brief Returns whether this provider supplies the default pipeline for scene and form packs. */
	virtual bool IsDefaultPipelineProvider() const { return false; }
	/** @brief Activates this provider's pipeline or restores its inactive pipeline. */
	virtual bool SetPipelineActive(bool) { return false; }
	/** @brief Returns the current payload's source root for transactional export. */
	virtual std::filesystem::path GetActiveRoot() const { return {}; }
	/** @brief Exports into a staged pack and updates only metadata owned by the provider. */
	virtual bool ExportTo(const std::filesystem::path&, nlohmann::json&, bool) { return false; }
	/** @brief Serializes this provider's feature handoff choices. */
	virtual nlohmann::json CaptureSceneControl() const { return nlohmann::json::object(); }
	/** @brief Applies manifest handoff choices, with personal overrides already merged. */
	virtual void ApplySceneControl(const nlohmann::json&) {}
	/** @brief Returns whether this provider can hand control of a feature to the preset settings layer. */
	virtual bool HandlesFeature(std::string_view) const { return false; }
	/** @brief Returns whether the provider currently yields the feature's controls. */
	virtual bool IsFeatureHandedOff(std::string_view) const { return false; }
	/** @brief Updates a runtime handoff choice without persisting the author's manifest. */
	virtual void SetFeatureHandedOff(std::string_view, bool) {}
	/** @brief Returns a feature whose full Baseline tree has a dedicated authoring checkbox. */
	virtual std::string_view GetDedicatedBaselineFeature() const { return {}; }
	/** @brief Returns whether the dedicated Baseline is compatible with an export's manifest type. */
	virtual bool CanExportBaseline(std::string_view) const { return true; }
	/** @brief Returns whether selected dedicated settings replace their complete Baseline tree. */
	virtual bool ReplacesBaselineTree() const { return false; }
};
