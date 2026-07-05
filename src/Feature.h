#pragma once

#include "FeatureCategories.h"
#include "FeatureConstraints.h"
#include "FeatureVersions.h"
#include "I18n/I18n.h"
#include "Utils/RestartSettings.h"

#include <cstring>
#include <span>
#include <string_view>
#ifdef TRACY_ENABLE
#	include <Tracy/Tracy.hpp>
#	include <Tracy/TracyD3D11.hpp>
#endif

struct Feature
{
	// For global settings search
	struct SettingSearchEntry
	{
		std::string label;
		std::string description;
		std::function<void()> focusCallback;  // Called to focus/highlight this setting in the UI
		std::string featureName;              // For display context
	};
	// Override in features to expose settings for search
	virtual std::vector<SettingSearchEntry> GetSettingsSearchEntries() { return {}; }

	// Restart-required settings introspection. Default: none.
	// Features with restart-gated fields override these to expose them to UI
	// helpers and MCP/RemoteControl without per-feature glue.
	virtual std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const { return {}; }
	/**
 * Retrieves the boot configuration value for a given JSON key.
 * @param jsonKey The JSON key identifying which boot value to retrieve.
 * @returns A pointer to the boot configuration value, or nullptr if not defined.
 */
	virtual const void* GetBootValue(std::string_view /*jsonKey*/) const { return nullptr; }
	/**
 * Retrieves the raw settings data blob.
 * @return Pointer to the settings blob data, or nullptr if unavailable.
 */
	virtual const void* GetSettingsBlob() const { return nullptr; }
	virtual size_t GetSettingsBlobSize() const { return 0; }

	// True if any restart-gated setting's live value differs from the
	// boot-latched value. Drives the green "RestartNeeded" tint in the
	// feature list and the `pending` flag in MCP's `list` response.
	bool HasAnyPendingRestart() const
	{
		const auto fields = GetRestartRequiredFields();
		if (fields.empty())
			return false;
		const auto* live = reinterpret_cast<const unsigned char*>(GetSettingsBlob());
		const size_t liveSize = GetSettingsBlobSize();
		if (!live || liveSize == 0)
			return false;
		for (const auto& field : fields) {
			if (!field.jsonKey || field.size == 0)
				continue;
			if (field.offset + field.size > liveSize)
				continue;
			const void* boot = GetBootValue(field.jsonKey);
			if (boot && std::memcmp(boot, live + field.offset, field.size) != 0)
				return true;
		}
		return false;
	}
	// Nexus Mods base URL for Skyrim Special Edition
	static constexpr std::string_view NEXUS_BASE_URL = "https://www.nexusmods.com/skyrimspecialedition/mods/";
	bool loaded = false;
	std::string version;
	std::string failedLoadedMessage;

	virtual std::string GetName() = 0;
	virtual std::string GetShortName() = 0;
	virtual std::string GetDisplayName() { return GetName(); }
	std::string GetDisplayCategory() const;
	virtual std::string GetFeatureModLink() { return ""; }
	virtual std::string_view GetShaderDefineName() { return ""; }

	/** @brief Gets additional shader define key-value pairs for this feature. */
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetShaderDefineOptions() { return {}; }

protected:
	/** @brief Builds a full Nexus Mods URL from a numeric mod ID. */
	static std::string MakeNexusModURL(std::string_view modId) noexcept
	{
		std::string url;
		url.reserve(NEXUS_BASE_URL.size() + modId.size());
		url.append(NEXUS_BASE_URL);
		url.append(modId);
		return url;
	}

public:
	virtual bool HasShaderDefine(RE::BSShader::Type) { return false; }
	/**
	 * Whether the feature supports VR.
	 *
	 * \return true if VR supported; else false
	 */
	virtual bool SupportsVR() { return false; }

	/**
	 * @brief Whether the feature is a CORE feature.
	 *
	 * Core features appear under "Core Features" in the UI. If a "CORE" file
	 * is present in the feature folder root, the feature is bundled into the
	 * main CS zip and automatically considered core.
	 */
	virtual bool IsCore() const
	{
		return FeatureVersions::FEATURE_CORE_NAMES.contains(const_cast<Feature*>(this)->GetShortName());
	}

	/**
	 * @brief Gets the category for UI grouping (e.g., "Terrain", "Lighting", "Characters").
	 *
	 * Core features are distributed to their respective categories.
	 */
	virtual std::string_view GetCategory() const { return FeatureCategories::kOther; }

	/**
	 * Release maturity stage, declared via the feature .ini (Alpha/Beta flags) and
	 * baked into FeatureVersions.h at build time. Alpha takes precedence over Beta.
	 */
	enum class ReleaseStage
	{
		Release,
		Beta,
		Alpha
	};

	virtual ReleaseStage GetReleaseStage() const
	{
		const auto name = const_cast<Feature*>(this)->GetShortName();
		if (FeatureVersions::FEATURE_ALPHA_NAMES.contains(name))
			return ReleaseStage::Alpha;
		if (FeatureVersions::FEATURE_BETA_NAMES.contains(name))
			return ReleaseStage::Beta;
		return ReleaseStage::Release;
	}

	bool IsAlpha() const { return GetReleaseStage() == ReleaseStage::Alpha; }
	bool IsBeta() const { return GetReleaseStage() == ReleaseStage::Beta; }

	/**
	 * Localized stage marker shown after the feature name ("[ALPHA]", "[BETA]"),
	 * empty for release features. Takes the stage so callers that already resolved
	 * it (see GetReleaseStage) avoid a redundant lookup.
	 */
	static std::string GetReleaseStageTag(ReleaseStage stage);

	/**
	 * Whether the feature is disabled at boot by default (before any user override).
	 * Alpha and Beta features start disabled on first install; users can still enable
	 * them via the "Disable at Boot" menu.
	 */
	virtual bool IsDisabledByDefault() const { return GetReleaseStage() != ReleaseStage::Release; }

	/**
	 * Whether the feature will show up in the GUI menu
	 */
	virtual bool IsInMenu() const { return true; }

	/**
	 * Whether to print the INI version missing message when this feature is unloaded
	 */
	virtual bool DrawFailLoadMessage() const { return true; }

	/**
	 * @brief Gets the feature summary and key features for hover tooltip and unloaded UI.
	 * @return Pair of {description, key feature bullet points}.
	 */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() { return {}; }

	/** @brief Allocates GPU resources (textures, buffers) needed by this feature. */
	virtual void SetupResources() {}

	/** @brief Releases and recreates transient state (e.g. on resolution change). */
	virtual void Reset() {}

	/**
	 * @brief Render-thread scene-transition reset (driven by LoadingMenu open/close).
	 *
	 * Dispatched by DrainSceneTransitions() on the render thread, so a feature can clear caches
	 * its menu or Prepass iterates without racing the main-thread LoadingMenu event. Prefer this
	 * over a direct MenuOpenCloseEvent sink whenever the reset touches render-thread-iterated state.
	 * @param opening true when the LoadingMenu is opening (old cell teardown), false when closing.
	 */
	virtual void OnSceneTransitionReset(bool /*opening*/) {}
	virtual void DrawSettings() {}

	/**
	 * @brief Renders this feature's VR performance-relevant controls into the central
	 * VR Performance panel. Default empty: features without VR perf knobs contribute
	 * nothing (fail-safe: no registry to keep in sync). Overrides should render the
	 * SAME controls (bound to the same settings) they show in their own panel, so the
	 * hub and the feature panel are two views of one state. The hub draws the section
	 * header (with a jump link to the feature's panel); overrides render controls only.
	 */
	virtual void DrawVRPerformanceSettings() {}

	/** @brief Section label the VR Performance hub draws (as a jump link to this
	 *         feature's panel) above this feature's controls. Override alongside
	 *         DrawVRPerformanceSettings; empty (default) draws no header. */
	virtual std::string GetVRPerformanceSectionLabel() { return ""; }

	/** @brief Sort key for the VR Performance hub (lower draws first); default puts
	 *         unranked features last so the order reflects perf impact, not registration. */
	virtual int GetVRPerformanceOrder() const { return 1000; }

	/** @brief Named VR performance profiles broadcast from the VR Performance hub. */
	enum class VRPerfProfile
	{
		Performance,  ///< Maximum framerate: lowest render res, all perf features on.
		Balanced,     ///< Middle ground.
		Quality       ///< Maximum fidelity: higher render res, perf shortcuts off.
	};

	/** @brief Shared profile convention: every VR reprojection feature enables reproject
	 *         except on Quality (max fidelity). One source so apply/match can't drift. */
	static constexpr bool VRProfileEnablesReproject(VRPerfProfile profile) { return profile != VRPerfProfile::Quality; }

	/**
	 * @brief Applies a VR performance profile to this feature's settings. Default empty:
	 * each feature maps the profile to its own settings (decoupled: the hub broadcasts
	 * one profile to every feature, none needs to know about the others). Restart-gated
	 * fields changed here surface their pending-restart banners as usual.
	 */
	virtual void ApplyVRPerformanceProfile(VRPerfProfile /*profile*/) {}

	/** @brief True when this feature's settings already equal what ApplyVRPerformanceProfile
	 *         would set for @p profile. The hub uses it to show the active profile (or Custom).
	 *         Default true so features without perf profiles don't veto the match. */
	virtual bool MatchesVRPerformanceProfile(VRPerfProfile /*profile*/) const { return true; }

	/** @brief Broadcasts a profile to every loaded feature. The hub button and the devbench
	 *         handler share this so the loaded-guard rule lives in exactly one place. */
	static void ApplyVRPerformanceProfileToAll(VRPerfProfile profile);

	/** @brief Draws the UI shown when this feature failed to load. */
	virtual void DrawUnloadedUI();

	/** @brief Per-frame work executed before the reflections pass. */
	virtual void ReflectionsPrepass() {};

	/** @brief Per-frame work executed before the main rendering pass. */
	virtual void Prepass() {}

	/** @brief Per-frame work executed before Prepass, earliest per-frame hook. */
	virtual void EarlyPrepass() {}

	/**
	 * @brief Called during disk-cache shader loading to generate additional shader permutations.
	 *
	 * Invoked once per BSShader load when the shader cache is in disk-cache mode.
	 * Features can override this to inject custom permutation descriptors into the
	 * shader cache so that feature-specific technique variants are compiled and stored.
	 * This is a cold path (disk I/O, not per-frame); performance is not critical here.
	 *
	 * @param shader The BSShader being loaded.
	 */
	virtual void GenerateShaderPermutations(RE::BSShader*) {}

	/** @brief Called during SKSE Load -- earliest hook point, only for critical hooks like D3D. */
	virtual void Load() {}

	/** @brief Called after all game data files have been loaded. */
	virtual void DataLoaded() {}

	/** @brief Called after all SKSE plugins have finished PostLoad. */
	virtual void PostPostLoad() {}

	/**
	 * @brief Loads the feature from its INI file and JSON settings.
	 *
	 * Validates the INI version against FeatureVersions, sets the loaded flag,
	 * and delegates to LoadSettings on success.
	 * @param o_json Root JSON object containing per-feature settings sections.
	 */
	void Load(json& o_json);

	void Save(json& o_json);
	virtual void SaveSettings(json&) {}
	virtual void LoadSettings(json&) {}
	virtual void RestoreDefaultSettings() {}

	/**
	 * @brief Toggles the "disabled at boot" state for this feature.
	 * @return The new disabled state (true = disabled at boot).
	 */
	virtual bool ToggleAtBootSetting();

	/**
	 * @brief Reapplies override settings for this feature if available
	 * @return True if overrides were found and applied, false otherwise
	 */
	virtual bool ReapplyOverrideSettings();

	/**
	 * Weather analysis configuration for features that want to provide weather analysis.
	 * If sectionName is empty, the feature will not appear in weather analysis UI.
	 * Features should populate this struct to opt-in to weather analysis display.
	 */
	struct WeatherAnalysisConfig
	{
		std::string sectionName;             // Display name for the collapsible section (empty = no weather analysis)
		std::function<void()> drawFunction;  // Custom draw function for weather analysis content

		// Constructor for easy initialization
		WeatherAnalysisConfig() = default;
		WeatherAnalysisConfig(const std::string& name, std::function<void()> drawFunc) :
			sectionName(name), drawFunction(std::move(drawFunc)) {}
	};

	/**
	 * Get weather analysis configuration for this feature.
	 * Returns empty sectionName by default (no weather analysis).
	 * Features should override this to provide their weather analysis section name and draw function.
	 */
	virtual WeatherAnalysisConfig GetWeatherAnalysisConfig() const { return {}; }

	/**
	 * @brief Called during feature initialization to register weather-controllable variables
	 * Features should register their weather variables here using the WeatherVariables::GlobalWeatherRegistry
	 * The weather system will automatically handle save/load/lerp for all registered variables
	 */
	virtual void RegisterWeatherVariables() {}

	/**
	 * @brief Returns constraints this feature imposes on other features' settings
	 *
	 * Features override this to declare runtime incompatibilities with other features.
	 * The constraint system will automatically:
	 * - Force the target setting to the specified value
	 * - Disable the UI control for the constrained setting
	 * - Show a tooltip explaining which features caused the constraint
	 *
	 * @return Vector of constraints this feature currently imposes (empty if none)
	 */
	virtual std::vector<FeatureConstraints::Constraint> GetActiveConstraints() const { return {}; }

	/**
	 * @brief Validates this feature's disk-cache entry against current install state.
	 * @param a_ini The cache INI to read from.
	 * @return True if the cache entry matches the current feature version and load state.
	 */
	virtual bool ValidateCache(CSimpleIniA& a_ini);

	/**
	 * @brief Writes this feature's version and enabled state into the disk-cache INI.
	 * @param a_ini The cache INI to write to.
	 */
	virtual void WriteDiskCacheInfo(CSimpleIniA& a_ini);

	/** @brief Invalidates any cached compiled shaders owned by this feature. */
	virtual void ClearShaderCache() {}

	static const std::vector<Feature*>& GetFeatureList();

	/**
	 * @brief Drains pending LoadingMenu transitions and dispatches OnSceneTransitionReset.
	 *
	 * Lazily registers a single LoadingMenu MenuOpenCloseEvent sink. The sink (main thread) only
	 * latches a flag; this drain runs the resets on the calling thread. Call once per frame from
	 * the render path so feature resets serialize with render-thread menu/Prepass iteration.
	 */
	static void DrainSceneTransitions();

	/**
	 * @brief Finds a loaded feature by its short name.
	 *
	 * @param shortName The short name to search for.
	 * @return Pointer to the feature if found and loaded, nullptr otherwise.
	 */
	static Feature* FindFeatureByShortName(const std::string& shortName);

	/**
	 * @brief Gets sorted short names of all loaded features that appear in the menu.
	 *
	 * @return Sorted vector of short name strings.
	 */
	static std::vector<std::string> GetLoadedFeatureNames();

	// Feature utility functions
	/**
	 * @brief Gets the minimum required version for a feature as a formatted string.
	 * @param shortName The short name of the feature.
	 * @return Formatted version string, or "unknown" if the feature is not in FEATURE_MINIMAL_VERSIONS.
	 */
	static std::string GetFeatureRequiredVersion(const std::string& shortName);

	/**
	 * @brief Checks if a feature has a minimum required version defined.
	 * @param shortName The short name of the feature.
	 * @param outVersion If non-null, receives the minimum version when found.
	 * @return True if the feature exists in FEATURE_MINIMAL_VERSIONS.
	 */
	static bool IsFeatureKnown(const std::string& shortName, REL::Version* outVersion = nullptr);

	// Injected once from State after TracyD3D11Context is created so ForEachLoadedFeature
	// can emit GPU timer zones without pulling in State headers here.
#ifdef TRACY_ENABLE
	inline static TracyD3D11Ctx s_tracyCtx = nullptr;

	/** @brief Sets the Tracy GPU context used by ForEachLoadedFeature for GPU profiling zones. */
	static void SetTracyCtx(TracyD3D11Ctx ctx) noexcept { s_tracyCtx = ctx; }
#endif

	/**
	 * @brief Invokes a callback on every loaded feature with optional Tracy profiling.
	 * @param methodName Label for the Tracy zone (e.g. "Reset", "Prepass").
	 * @param callback Callable receiving a Feature* for each loaded feature.
	 * @param emitGpuZone When true and Tracy is enabled, also emits a GPU timer zone.
	 */
	template <typename Func>
	static inline void ForEachLoadedFeature(std::string_view methodName, Func&& callback, bool emitGpuZone = false)
	{
		for (auto* feature : GetFeatureList()) {
			if (feature->loaded) {
#ifdef TRACY_ENABLE
				{
					const auto zoneName = std::format("{}::{}", feature->GetShortName(), methodName);
					ZoneTransientN(___tracy_feature_zone, zoneName.c_str(), true);
					if (emitGpuZone) {
						TracyD3D11ZoneTransientS(s_tracyCtx, ___tracy_d3d11_feature_zone, zoneName.c_str(), 0, s_tracyCtx != nullptr);
						callback(feature);
					} else {
						callback(feature);
					}
				}
#else
				callback(feature);
#endif
			}
		}
	}
};
