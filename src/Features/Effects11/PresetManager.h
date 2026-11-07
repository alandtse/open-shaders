#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

enum class PresetLocationKind : uint8_t
{
	GameRoot,      // <gameroot>\enbseries.ini
	DataRoot,      // <gameroot>\Data\enbseries.ini
	Library,       // portable preset library
	DataSubfolder  // <gameroot>\Data\<mod name>\enbseries.ini
};

/** @brief One of Effects11's optional .fx-backed effects: an enbseries.ini [EFFECT]
 *  flag and whether the file it would load actually exists in this preset. */
struct PresetEffectStatus
{
	std::string flagName;
	bool enabled = false;
	bool fileExists = false;
};

/** @brief A pointer/reference into this is invalidated by the next Rescan(). */
struct PresetLocation
{
	PresetLocationKind kind;
	std::filesystem::path root;  // dir containing enbseries.ini and enbseries folder
	std::string label;           // display label for the picker
	bool valid = false;          // exists(root / "enbseries.ini") && is_directory(root / "enbseries")

	std::string headerComment;                     // enbeffect.fx's leading // comment block, reproduced verbatim; empty if none found
	std::vector<PresetEffectStatus> effectStatus;  // ini flag vs actual file, for the picker tooltip

	PresetLocation(PresetLocationKind a_kind, std::filesystem::path a_root, std::string a_label, bool a_valid) :
		kind(a_kind), root(std::move(a_root)), label(std::move(a_label)), valid(a_valid) {}
};

class PresetManager
{
public:
	static PresetManager& GetSingleton();

	/** @brief Re-scans game root, Data root, and Data's immediate child folders for
	 *  enbseries.ini + enbseries\ pairs. Call at Feature Initialize() and from an
	 *  explicit UI Rescan action; do not call every frame. */
	void Rescan();

	const std::vector<PresetLocation>& GetDiscoveredLocations() const;

	/** @brief root must be the .root of one of GetDiscoveredLocations()'s entries
	 *  (or empty, meaning no active location). Only changes cached state; does NOT
	 *  reload settings or reapply effects -- the caller does that (see MenuManager). */
	void SetActiveLocation(const std::filesystem::path& root);
	/** @brief Currently active location, or nullptr if none is active. */
	const PresetLocation* GetActiveLocation() const;

	/** @brief Converts a discovered location's (absolute) root to a path relative to
	 *  the game root, safe to persist -- an absolute path would break if the game
	 *  folder ever moves (Steam library migration, syncing configs across machines). */
	std::string ToRelativeKey(const std::filesystem::path& a_root) const;
	/** @brief Reverse of ToRelativeKey(): the discovered location whose root matches
	 *  a_relativeKey, or nullptr. */
	const PresetLocation* FindByRelativeKey(const std::string& a_relativeKey) const;

	// Unchanged signatures, still the only thing ~20 other call sites (WeatherManager,
	// Effect, SettingManager, Effects11.cpp) need to know about.
	std::filesystem::path GetENBSeriesPath() const;
	std::filesystem::path GetENBSeriesIniPath() const;

	/** @brief Empty string identifies the Legacy (root / Data) install. */
	static constexpr const char* kLegacyPresetId = "";

	/** @brief Prefix for unified-pack effects11 folders registered in the library. */
	static constexpr const char* kUnifiedPackIdPrefix = "pack:";

	static constexpr const char* kEnbSeriesDirName = "enbseries";
	static constexpr const char* kEnbSeriesIniName = "enbseries.ini";
	/** @brief Required FX file for a valid library preset (matches ENBEffect::GetName()). */
	static constexpr const char* kRequiredEffectFile = "enbeffect.fx";

	/** @brief One discovered preset, valid or not. */
	struct PresetInfo
	{
		std::string id;  ///< Empty for Legacy; folder name or pack:id for library
		std::string displayName;
		std::filesystem::path rootPath;  ///< Contains enbseries.ini + enbseries/
		bool valid = true;
		std::string invalidReason;
		bool isLegacy = false;
		bool isUnifiedPack = false;
	};

	/** @brief Rescans Legacy, library and unified pack folders and repairs an invalid active selection. */
	void DiscoverPresets();
	/**
	 * @brief Lists Legacy first (invalid when not installed), then library presets and, when includeUnifiedPacks,
	 *        unified packs' effects11 folders, sorted by name. Leaves the discovered list and the selection alone.
	 */
	std::vector<PresetInfo> ScanPresets(bool includeUnifiedPacks) const;
	/** @brief Every preset found by the last DiscoverPresets. */
	const std::vector<PresetInfo>& GetPresets() const { return presets; }

	/** @brief The selected preset's id; kLegacyPresetId for Legacy. */
	const std::string& GetActivePresetId() const { return activePresetId; }

	/** @brief Selects the first valid library or unified pack preset, falling back to Legacy when none exists. */
	void SelectDefaultPreset();

	/** @brief Selects a discovered preset without reloading FX.
	 *  @return False if the id is unknown or invalid. */
	bool SetActivePreset(const std::string& id);

	/** @brief Whether an enbseries.ini and enbseries/ exist under Data or the game root. */
	bool HasLegacyInstall() const;
	/** @brief Whether the Legacy install is the active preset. */
	bool IsLegacyActive() const { return activePresetId == kLegacyPresetId; }
	/** @brief Absolute Legacy enbseries.ini path. */
	std::filesystem::path GetLegacyENBSeriesIniPath() const;

	/** @brief Hotswap: optionally saves the current preset, then reloads settings and weather and recompiles
	 *         ENB FX for id. Does not clear the Community Shaders shader cache. */
	bool SwitchPreset(const std::string& id, bool saveCurrent = true);

	/** @brief Reloads settings and weather and recompiles ENB FX for the active preset. */
	void ReloadActive();

	/** @brief Library id for a unified pack's effects11 folder. */
	static std::string MakeUnifiedPackPresetId(const std::string& packId);

	/** @brief Whether presetRoot has enbseries.ini and enbseries/enbeffect.fx; outReason says what is missing. */
	static bool ValidateLibraryPreset(const std::filesystem::path& presetRoot, std::string& outReason);

	/** @brief Folder that currently holds the active enbseries.ini + enbseries/ (library, pack, or Legacy). */
	std::filesystem::path GetActivePresetRoot() const;

	/** @brief Whether the active selection has a valid ENBSeries layout that can be exported. */
	bool CanExportActivePreset() const;

	/**
	 * @brief Saves the live ENB settings (optional) and copies enbseries.ini + enbseries/ into destRoot.
	 *  Only those ENB files are copied, never the whole Legacy game/Data root. No-ops when source == dest.
	 */
	bool ExportActivePresetTo(const std::filesystem::path& destRoot, bool saveCurrent = true);

	/** @brief Resolves the feature-owned payload in a portable pack. */
	static std::filesystem::path ResolvePackRoot(const std::filesystem::path&, const nlohmann::json&);

private:
	/** @brief Whether the Legacy install lives under Data rather than the game root. */
	bool UseDataFolder() const;
	/** @brief Absolute Legacy enbseries directory. */
	std::filesystem::path GetLegacyENBSeriesPath() const;

	/** @brief Library preset root when a valid non-Legacy preset is active; empty otherwise. */
	std::filesystem::path GetActiveLibraryRoot() const;

	/** @brief Whether the ini exists and the series folder is a directory. */
	static bool IsValidEnbSeriesLayout(const std::filesystem::path& iniPath, const std::filesystem::path& seriesDir);
	/** @brief The id, or "Legacy" for the empty Legacy id. */
	static std::string FormatPresetIdForLog(const std::string& id);

	/** @brief The discovered preset with this id, or null. */
	const PresetInfo* FindPreset(const std::string& id) const;
	/** @brief Keeps a valid selection, else applies SelectDefaultPreset. */
	void EnsureDefaultSelection();
	/** @brief Adds each preset folder under a CommunityShaders subfolder, across the VFS and real mod roots. */
	static void ScanLibraryDirectory(const std::filesystem::path& relativePath, bool unifiedPacks, std::vector<PresetInfo>& out);

	std::vector<PresetInfo> presets;
	std::string activePresetId = kLegacyPresetId;
	bool discovered = false;

	std::vector<PresetLocation> discoveredLocations;
	std::filesystem::path activeRoot;  // empty = none active
};
