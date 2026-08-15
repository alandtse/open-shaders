// ShadowCasterManager.cpp
// Shadow caster scheduling for LightLimitFix.
//
// Based on Intellightent by meh321
//   https://www.nexusmods.com/skyrimspecialedition/mods/172423
//
// Ported and adapted for Community Shaders by the Community Shaders team with permission.

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "../../Utils/PerfUtils.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "../VR.h"
#include "I18n/I18n.h"
#include "ShadowCasterInternal.h"

#include <Windows.h>  // SEH (__try) for the shadow-light usability backstop
#include <mutex>
#include <shared_mutex>

#define I18N_KEY_PREFIX "feature.light_limit_fix."

namespace ShadowCasterManager
{
	// =========================================================================
	// Module-level state (declarations + docs in ShadowCasterInternal.h)
	// =========================================================================

	/// Total LightEntry slots: sun (1) + shadow casters (>=4) + converted pool.
	static int32_t LightContainerSize(const Settings& s)
	{
		return std::max(4, s.ShadowLightCount) + 1 + s.ConvertedShadowSlots;
	}

	Settings s_settings;
	LightContainer s_lights;
	BudgetTracker s_budget;

	bool s_externalConflict = false;
	std::string s_conflictMessage;

	int s_focusShadowSlots = 0;

	int32_t s_redrawHistory[kRedrawHistorySize] = {};
	int32_t s_redrawHistoryPos = 0;
	int32_t s_redrawSum = 0;

	int32_t s_budgetHistory[kRedrawHistorySize] = {};
	int32_t s_budgetHistoryPos = 0;
	int64_t s_budgetSum = 0;

	float s_ftRing[kFrameWindow]{};
	int s_ftHead = 0;
	int s_ftCount = 0;
	float s_ftEMA = 0.0f;
	int s_stableFrames = 0;
	float s_autoBudgetMs = 0.0f;

	int32_t s_redrawnLightsThisFrame = 0;
	int32_t s_totalShadowLightsThisFrame = 0;
	uint32_t s_highImportanceLightCount = 0;
	float s_redrawnLightsSmoothed = 0.0f;

	ShadowDemandSample s_shadowDemand{};

	std::atomic<uint32_t> s_alphaGroupPeak{ 0 };
	std::atomic<uint64_t> s_alphaGroupDrops{ 0 };

	SchedDiagCounters s_schedDiag;

	int32_t s_installedShadowLightCount;
	uint32_t s_requestedSlotCount = 0;
	uint32_t s_installedSlotCount = 0;
	bool s_slotCountLogged = false;

	std::unique_ptr<FormulaHelper> s_formulaScore;
	std::unique_ptr<FormulaHelper> s_formulaRedrawInterval;
	std::unique_ptr<FormulaHelper> s_formulaRedrawBudget;

	std::vector<ConvertedLight> s_normalConvert;
	std::set<RE::NiLight*> s_shadowConvert;
	std::set<RE::NiLight*> s_shadowConvertDescriptorInited;
	std::mutex s_shadowConvertMutex;

	std::atomic<bool> s_pendingSessionReset{ false };
	std::atomic<bool> s_pendingCellReset{ false };

	std::shared_mutex s_portalGraphMutex;
	std::shared_mutex s_lightsPoolMutex;

	std::atomic<int> s_shadowFlushReaders{ 0 };
	std::atomic<bool> s_teardownWaiting{ false };

	std::unordered_set<uintptr_t> s_suppressedLights;

	// Lights the hovered table group selects this frame; the cluster builder
	// reads it to magenta-tint them. Rebuilt each schedule (render thread), read
	// on the same thread -- same access pattern as s_suppressedLights. Any
	// grouping (below-floor, converted, promoted, ...) populates this one set.
	std::unordered_set<uintptr_t> s_highlightLights;

	// Lights the Light Impact Floor actually culled this frame (empty while the
	// floor is 0). Rebuilt each schedule on the render thread, read there by the
	// table's "Low" group -- same access pattern as s_highlightLights.
	std::unordered_set<uintptr_t> s_belowFloorLights;

	std::unordered_set<uintptr_t> s_pinShadow;
	std::unordered_set<uintptr_t> s_pinConvert;
	uintptr_t s_soloLight = 0;
	uintptr_t s_hoverLightKey = 0;

	// =========================================================================
	// Public API
	// =========================================================================

	void Init(const Settings& settings)
	{
		s_settings = settings;

		// Check for external shadow management plugins that conflict with our hooks.
		if (GetModuleHandleW(L"intellightent-ng.dll")) {
			s_externalConflict = true;
			s_conflictMessage =
				"Disabled: intellightent-ng.dll detected. Both mods manage shadow caster "
				"selection and cannot run simultaneously. Remove one to use the other.";
			logger::warn("[SCM] {}", s_conflictMessage);
			return;
		}

		int total = LightContainerSize(settings);
		s_lights.Size = total;
		s_lights.Sun = false;
		s_lights.Lights = new LightEntry[total]();
		for (int i = 0; i < total; i++)
			s_lights.Lights[i].Index = i;

		// Seed auto-budget ring buffer to 60 fps so the first few frames have sane values.
		std::fill(std::begin(s_ftRing), std::end(s_ftRing), 16.67f);
		s_ftEMA = 16.67f;

		// Parse formula strings
		if (!settings.ScoreFormula.empty()) {
			s_formulaScore = std::make_unique<FormulaHelper>();
			if (!s_formulaScore->Parse(settings.ScoreFormula))
				logger::error("[SCM] Failed to parse ScoreFormula");
		}
		if (!settings.RedrawIntervalFormula.empty()) {
			s_formulaRedrawInterval = std::make_unique<FormulaHelper>();
			if (!s_formulaRedrawInterval->Parse(settings.RedrawIntervalFormula))
				logger::error("[SCM] Failed to parse RedrawIntervalFormula");
		}
		if (!settings.RedrawBudgetFormula.empty()) {
			s_formulaRedrawBudget = std::make_unique<FormulaHelper>();
			if (!s_formulaRedrawBudget->Parse(settings.RedrawBudgetFormula))
				logger::error("[SCM] Failed to parse RedrawBudgetFormula");
		}
	}

	// Set by the resolution combo when the user picks a new tier. Gates the
	// SaveINISettings write so we only touch SkyrimPrefs.ini when there's an
	// actual change to persist -- without this, every Save Settings click
	// would rewrite the user's prefs file even if shadow res wasn't edited.
	bool s_shadowResolutionDirty = false;

	// Set by the shadow-distance sliders (fInteriorShadowDistance / fShadowDistance).
	// Unlike resolution these apply live (the engine reads them each frame for shadow
	// culling), but still need persisting to SkyrimPrefs.ini on Save Settings.
	bool s_shadowDistanceDirty = false;

	void LoadINISettings()
	{
		// No-op: the engine already loaded SkyrimPrefs.ini at startup, so the
		// live RE::Setting reflects the user's saved value. Future overrides
		// that need to land before SCM::Install hook here.
	}

	// Refresh the engine's cached shadow-cull distance after a slider edit so it
	// applies without a cell reload.
	void RefreshEngineShadowDistanceCache()
	{
		CallUpdateShadowDistance(Util::IsInterior());
	}

	// Persist one live INIPref setting to SkyrimPrefs.ini.
	//
	// The engine's INIPrefSettingCollection::WriteSetting requires OpenHandle to
	// have been called first (it writes via the cached `handle` member, which is
	// null between RefreshINI calls). Calling it directly returns true but silently
	// no-ops -- verified by the fact that the live RE::Setting updates but
	// SkyrimPrefs.ini's timestamp doesn't change after Save Settings. Sidestep the
	// engine path entirely with WritePrivateProfileStringA. CommonLib stores the
	// full path of SkyrimPrefs.ini in subKey at startup (see
	// InitializeSkyrimINIPrefSettingCollection caller at SE 1406489e6 / AE 140648990
	// / VR equivalent). The setting name encodes "<key>:<section>", and the key's
	// type prefix (i/u/f/b) picks the value formatting.
	static void PersistPrefSetting(RE::INIPrefSettingCollection* prefColl, RE::Setting* setting)
	{
		if (!prefColl || !setting)
			return;
		const char* fullName = setting->GetName();
		const char* colon = std::strchr(fullName, ':');
		if (!colon) {
			logger::warn("[SCM] Setting name '{}' has no section -- cannot write to INI", fullName);
			return;
		}
		const std::string key(fullName, colon - fullName);
		const std::string section(colon + 1);
		std::string value;
		switch (fullName[0]) {
		case 'f':
			value = std::to_string(setting->GetFloat());
			break;
		case 'b':
			value = setting->GetBool() ? "1" : "0";
			break;
		default:  // i / u and anything else -> integer
			value = std::to_string(setting->GetInteger());
			break;
		}

		// subKey holds the full path to SkyrimPrefs.ini.
		const char* iniPath = prefColl->subKey;
		if (!iniPath || !iniPath[0]) {
			logger::warn("[SCM] INIPrefSettingCollection subKey is empty -- cannot write to INI");
			return;
		}

		if (::WritePrivateProfileStringA(section.c_str(), key.c_str(), value.c_str(), iniPath)) {
			// Windows caches INI writes in-process; the file on disk doesn't
			// update until the cache is flushed. Calling WritePrivateProfile
			// with three NULL parameters forces the flush. Without this the
			// write succeeds (returns non-zero, no error) but the file's
			// timestamp and contents stay stale until the process exits.
			// See KB Q104112 / MSDN remarks for WritePrivateProfileString.
			::WritePrivateProfileStringA(nullptr, nullptr, nullptr, iniPath);
			// Log the setting only -- iniPath holds the user's profile dir.
			logger::info("[SCM] Persisted [{}]{}={}", section, key, value);
		} else {
			const DWORD err = ::GetLastError();
			logger::warn("[SCM] WritePrivateProfileStringA failed (err={}) writing [{}]{}={}",
				err, section, key, value);
		}
	}

	// Resolve a "<key>:<section>" engine setting from whichever collection owns it.
	// The light-LOD knobs live in Skyrim.ini (INISettingCollection) while the shadow
	// distances live in SkyrimPrefs.ini (INIPrefSettingCollection); callers shouldn't
	// have to know which. Persisting always targets SkyrimPrefs.ini, which overrides
	// Skyrim.ini at load, so a single write path keeps either kind sticky.
	RE::Setting* GetDisplaySetting(const char* a_name)
	{
		if (auto* pc = RE::INIPrefSettingCollection::GetSingleton())
			if (auto* s = pc->GetSetting(a_name))
				return s;
		if (auto* ic = globals::game::iniSettingCollection)
			if (auto* s = ic->GetSetting(a_name))
				return s;
		return nullptr;
	}

	void SaveINISettings()
	{
		if (!s_shadowResolutionDirty && !s_shadowDistanceDirty)
			return;
		auto* prefColl = RE::INIPrefSettingCollection::GetSingleton();
		if (!prefColl)
			return;

		if (s_shadowResolutionDirty) {
			PersistPrefSetting(prefColl, prefColl->GetSetting("iShadowMapResolution:Display"));
			s_shadowResolutionDirty = false;
		}
		if (s_shadowDistanceDirty) {
			PersistPrefSetting(prefColl, prefColl->GetSetting("fInteriorShadowDistance:Display"));
			PersistPrefSetting(prefColl, prefColl->GetSetting("fShadowDistance:Display"));
			// Light-LOD knobs live in Skyrim.ini; persist to SkyrimPrefs.ini anyway
			// (it overrides at load) so the master Light Fade Distance sticks.
			PersistPrefSetting(prefColl, GetDisplaySetting("fLightLODStartFade:Display"));
			PersistPrefSetting(prefColl, GetDisplaySetting("fLightLODMaxStartFade:Display"));
			s_shadowDistanceDirty = false;
		}
	}

	// Boot-time value of settings.Enabled, captured once in Install() and
	// never modified afterwards. The ImGui "Restart required" label
	// compares the user's current setting against this rather than the
	// (mutable) s_settings.Enabled, so the label persists across Save
	// Settings clicks until the user actually restarts. Without this the
	// label vanished the moment they saved -- s_settings would catch up
	// to the new value and the !=-against-staged condition cleared.
	bool s_bootEnabled = false;
	bool s_bootEnabledCaptured = false;
	bool s_bootAtlasEnabled = false;
	Util::Settings::BootSnapshot<Settings> s_bootSnapshot{ kRestartFields };

	void SetShadowDemand(const ShadowDemandSample& sample)
	{
		s_shadowDemand = sample;
	}

	void Update(const Settings& settings, RE::ShadowSceneNode* /*shadowSceneNode*/,
		RE::NiCamera* /*worldCamera*/)
	{
		ZoneScopedN("SCM::Update");
		if (s_externalConflict)
			return;

		// Lazy verification of the kSHADOWMAPS allocation. Self-healing:
		// retries until kSHADOWMAPS exists, then early-returns. Cheap.
		// This must run BEFORE the clamp below so a VRAM-exhaustion
		// fallback gets reflected in the same frame the verification
		// succeeds.
		RefreshInstalledSlotCount();

		Settings capped = settings;
		if (s_installedShadowLightCount > 0)
			capped.ShadowLightCount = std::min(settings.ShadowLightCount, s_installedShadowLightCount);

		// Until the atlas resources exist (or if the probe failed), the
		// vanilla 8-slice array is the only shadow storage; scheduling
		// beyond it would index slices the engine never allocated.
		if (s_bootAtlasEnabled && !AtlasActive())
			capped.ShadowLightCount = std::min(capped.ShadowLightCount, kVanillaShadowSliceCount);

		int newTotal = LightContainerSize(capped);
		if (newTotal != s_lights.Size) {
			auto* newLights = new LightEntry[newTotal]();
			// Exclusive against ScheduleShadowCasters/RenderScheduledShadowLights'
			// shared lock: the copy below reads live entries the scheduler can
			// concurrently write under its shared_lock, so the lock must cover
			// the copy, not just the pointer swap.
			std::unique_lock poolLock(s_lightsPoolMutex);
			auto* oldLights = s_lights.Lights;
			int copyCount = std::min(s_lights.Size, newTotal);
			for (int i = 0; i < copyCount; i++)
				newLights[i] = oldLights[i];
			for (int i = copyCount; i < newTotal; i++)
				newLights[i].Index = i;
			s_lights.Lights = newLights;
			s_lights.Size = newTotal;
			poolLock.unlock();
			delete[] oldLights;
		}

		// Apply settings as a pure flag flip. Conversion-related state
		// (s_normalConvert, s_shadowConvert, s_lights pool) is NOT
		// drained on toggles -- it ages out at the next LoadingMenu
		// via the natural ResetSession() in SceneTransitionEventHandler.
		// See Hook_CalculateActiveShadowCasters::thunk for the rationale:
		// wholesale clearing mid-session caused engine accumulate-shadow
		// crashes (2026-05-17 crash logs) because the engine still had
		// our converted/promoted lights in activeShadowLights with
		// half-populated shadowmapDescriptors, and tearing our tracking left
		// the engine walking that half-state.
		//
		// Each setting's gating still takes effect immediately via the
		// runtime checks in the relevant hook / scheduler branches:
		//   - Enabled: per-frame thunk routes to vanilla; OverwriteShadowMapIndex no-ops.
		//   - ConvertExcessToNormal: convertOrDisable routes excess omnis to DisableLight.
		//   - PromoteNormalToShadow: Hook_ConvertLights_Add stops promoting.
		// "Off = stop converting" is the documented semantic; existing
		// converted/promoted lights persist in their current form until
		// the engine itself drops them at cell change.

		// Reparse whenever the source string changes, regardless of writer;
		// invalid formulas fail closed (keep the previous compiled expression).
		auto reparseIfChanged = [](std::unique_ptr<FormulaHelper>& helper, const std::string& oldFormula,
									const std::string& newFormula, const char* label) {
			if (newFormula == oldFormula)
				return;
			auto candidate = std::make_unique<FormulaHelper>();
			if (candidate->Parse(newFormula))
				helper = std::move(candidate);
			else
				logger::error("[SCM] Failed to parse {} (external update); keeping previous formula", label);
		};
		reparseIfChanged(s_formulaScore, s_settings.ScoreFormula, capped.ScoreFormula, "ScoreFormula");
		reparseIfChanged(s_formulaRedrawInterval, s_settings.RedrawIntervalFormula, capped.RedrawIntervalFormula, "RedrawIntervalFormula");
		reparseIfChanged(s_formulaRedrawBudget, s_settings.RedrawBudgetFormula, capped.RedrawBudgetFormula, "RedrawBudgetFormula");

		s_settings = capped;
	}

	void ResetSession()
	{
		FreeAllTiles();
		// Wholesale drop of pointers the engine is about to free during
		// a scene transition. Called from LightLimitFix::OnSceneTransitionReset
		// on the render thread when the LoadingMenu opens. The per-frame reconciliation in
		// ScheduleShadowCasters keeps these caches honest during normal
		// play; this is the explicit "scene is gone" signal so the UI
		// counter, debug pins, and tracking sets read empty during the
		// loading screen rather than displaying stale entries from the
		// previous cell.
		//
		// Stale BSRenderPass.sceneLights[] captures that would otherwise AV
		// in BSEffectShader::SetupGeometry are handled by the defensive
		// guard there (clamps numLights past the first stale entry), not by
		// trying to drive engine-side cleanup from here. An earlier version
		// tried calling ShadowSceneNode::RemoveLight to undo our
		// ConvertLight -> GameEnableLight pinning, but the engine function
		// takes NiLight* (not BSLight* as the wrapper assumed); the call
		// was a silent no-op on every runtime and accomplished nothing.
		s_normalConvert.clear();
		// Promotion tracking is NOT wiped here: this reset is deferred to the render thread
		// and can drain after the game thread promotes the new cell's lights, wiping live
		// entries. ScheduleShadowCasters reconciles stale entries per-frame instead.
		s_pinShadow.clear();
		s_pinConvert.clear();
		s_soloLight = 0;
		s_suppressedLights.clear();
		// Demand is measured per pool slot; a new scene's light at the same slot
		// index must not inherit the previous occupant's redraw deprioritization.
		s_shadowDemand = ShadowDemandSample{};
		// Clear pool entries but keep the array allocation; size is set by
		// Install/Update based on the configured ShadowLightCount.
		if (s_lights.Lights) {
			for (int i = 0; i < s_lights.Size; ++i)
				s_lights.Lights[i].Clear();
			s_lights.Sun = false;
		}
	}

	const LightContainer& GetLights()
	{
		return s_lights;
	}

	bool IsActive()
	{
		// Boot-latched, not the live s_settings.Enabled: hooks install only when
		// enabled at boot, and runtime toggles are restart-gated.
		return s_bootEnabled && !s_externalConflict;
	}

	// Engine's native kSHADOWMAPS slice from the live descriptor, for when SCM
	// is inactive and its pool is empty. Sun (-> kSHADOWMAPS_ESRAM) and lights
	// without a descriptor return -1 so callers skip them.
	static int32_t GetEngineShadowSlot(RE::BSShadowLight* light)
	{
		if (!light || light->GetIsDirectionalLight())
			return -1;
		if (globals::game::isVR) {
			auto& rd = light->GetVRRuntimeData();
			if (rd.shadowmapDescriptors.empty())
				return -1;
			return static_cast<int32_t>(rd.shadowmapDescriptors[0].shadowmapIndex);
		}
		auto& rd = light->GetRuntimeData();
		if (rd.shadowmapDescriptors.empty())
			return -1;
		return static_cast<int32_t>(rd.shadowmapDescriptors[0].shadowmapIndex);
	}

	int32_t GetShadowSlot(RE::BSShadowLight* light)
	{
		// Returns the kSHADOWMAPS texture-array slot for `light`, or -1 if the
		// light has no kSHADOWMAPS slice. Pool index == texture slot for point
		// lights (1:1). Sun's pool slot returns -1 since the sun renders to
		// kSHADOWMAPS_ESRAM (a separate texture) — callers in ShadowRenderer
		// upload and LightLimitFix cluster builder must skip it.
		//
		// With SCM disabled at boot the pool is empty; without this fallback
		// every caster returns -1 and the cluster builder drops it (#97).
		if (!IsActive())
			return GetEngineShadowSlot(light);

		const int32_t poolIdx = s_lights.FindLight(light, s_settings.ShadowLightCount);
		if (poolIdx < 0)
			return -1;
		if (s_lights.Sun && poolIdx == 0)
			return -1;  // sun
		return poolIdx;
	}

	float GetRenderedTileScale(int32_t poolSlot)
	{
		// Slot content stays at the scale it was last rasterized at, so the
		// upload must keep advertising renderedScale even while the tiles
		// setting is off or pendingScale differs -- until the redraws catch up,
		// the slice really does hold a tile.
		if (!s_lights.Lights || poolSlot < 0 || poolSlot >= s_lights.Size)
			return 1.0f;
		return s_lights.Lights[poolSlot].renderedScale;
	}

	float GetShadowFadeAlpha(int32_t poolSlot)
	{
		if (!s_lights.Lights || poolSlot < 0 || poolSlot >= s_lights.Size)
			return 1.0f;
		const double start = s_lights.Lights[poolSlot].FadeStartSeconds;
		if (start < 0.0)
			return 1.0f;
		const float elapsedSeconds = static_cast<float>(Util::GetNowSecs() - start);
		return std::clamp(elapsedSeconds / kShadowFadeInSeconds, 0.0f, 1.0f);
	}

	void ForEachConvertedLight(const std::function<void(RE::BSShadowLight*)>& visitor)
	{
		for (auto& c : s_normalConvert) {
			if (!c.light)
				continue;
			// Defensive vtable check: catches lights freed and zeroed by
			// tbbmalloc / EngineFixes between our per-frame reconciliation
			// in ScheduleShadowCasters and the cluster builder running. The
			// reconciliation prunes stale pointers up-front, but a bulk
			// engine teardown could still happen mid-frame.
			if (*reinterpret_cast<const uintptr_t*>(c.light) == 0)
				continue;
			visitor(c.light);
		}
	}

	// =========================================================================
	// Per-slot visualization state (owned by ShadowCasterManager)
	// =========================================================================

	std::vector<ShadowSlotInfo> s_shadowSlotInfos;
	uint32_t s_shadowSlotUsage = 0;
	// Persists last-seen ShadowSlotInfo for every light ever recorded this session,
	// so suppressed lights that leave the active slots still have metadata for the settings table.
	std::unordered_map<uintptr_t, ShadowSlotInfo> s_knownLights;

	/// Computes the golden-ratio hue color for a shadow-map slot (matches mode-8 shader).
	ImVec4 ShadowSlotHueColor(uint32_t slotIdx)
	{
		auto chan = [](float h, float shift) {
			float v = fmodf(h + shift, 1.0f);
			if (v < 0.0f)
				v += 1.0f;
			return std::clamp(fabsf(v * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
		};
		float hue = fmodf(float(slotIdx) * 0.618033988f, 1.0f);
		return ImVec4(chan(hue, 0.0f), chan(hue, 2.0f / 3.0f), chan(hue, 1.0f / 3.0f), 1.0f);
	}

	// =========================================================================
	// Slot frame API implementations
	// =========================================================================

	void BeginSlotFrame(uint32_t slotCount)
	{
		s_shadowSlotInfos.assign(slotCount, ShadowSlotInfo{});
		s_shadowSlotUsage = 0;
	}

	void RecordSlot(uint32_t depthSlot, const ShadowSlotInfo& info)
	{
		if (depthSlot < static_cast<uint32_t>(s_shadowSlotInfos.size()))
			s_shadowSlotInfos[depthSlot] = info;
		// Omni lights (type 2) occupy 2 depth-texture slices; all others use 1.
		s_shadowSlotUsage += (info.type == 2) ? 2 : 1;
		s_knownLights[info.lightKey] = info;
	}

	bool IsSuppressed(uintptr_t lightKey)
	{
		if (s_suppressedLights.count(lightKey))
			return true;
		// Solo: every key except the soloed one is implicitly suppressed.
		if (s_soloLight != 0 && s_soloLight != lightKey)
			return true;
		return false;
	}

	bool IsPinnedShadow(uintptr_t lightKey) { return s_pinShadow.count(lightKey) > 0; }
	bool IsPinnedConvert(uintptr_t lightKey) { return s_pinConvert.count(lightKey) > 0; }

	void SetPinnedShadow(uintptr_t lightKey, bool pinned)
	{
		if (pinned) {
			s_pinShadow.insert(lightKey);
			s_pinConvert.erase(lightKey);  // mutually exclusive
			s_suppressedLights.erase(lightKey);
		} else {
			s_pinShadow.erase(lightKey);
		}
	}

	void SetPinnedConvert(uintptr_t lightKey, bool pinned)
	{
		if (pinned) {
			s_pinConvert.insert(lightKey);
			s_pinShadow.erase(lightKey);
			s_suppressedLights.erase(lightKey);
		} else {
			s_pinConvert.erase(lightKey);
		}
	}

	uintptr_t GetSoloLight() { return s_soloLight; }
	void SetSoloLight(uintptr_t lightKey) { s_soloLight = lightKey; }

	uintptr_t GetHoveredLight() { return s_hoverLightKey; }
	void SetHoveredLight(uintptr_t lightKey) { s_hoverLightKey = lightKey; }

	void ClearHighlight() { s_highlightLights.clear(); }
	void AddHighlight(uintptr_t lightKey) { s_highlightLights.insert(lightKey); }
	bool IsHighlighted(uintptr_t lightKey) { return s_highlightLights.count(lightKey) != 0; }

	void ClearBelowFloor() { s_belowFloorLights.clear(); }
	void AddBelowFloor(uintptr_t lightKey) { s_belowFloorLights.insert(lightKey); }
	bool IsBelowFloor(uintptr_t lightKey) { return s_belowFloorLights.count(lightKey) != 0; }

	void ClearAllOverrides()
	{
		s_suppressedLights.clear();
		s_pinShadow.clear();
		s_pinConvert.clear();
		s_soloLight = 0;
		// Hover key is transient (per-draw); not part of "overrides".
	}

	bool HasAnyOverrides()
	{
		return !s_suppressedLights.empty() || !s_pinShadow.empty() ||
		       !s_pinConvert.empty() || s_soloLight != 0;
	}

	bool HasSuppressedLights()
	{
		return !s_suppressedLights.empty();
	}

	uint32_t GetSlotUsage()
	{
		return s_shadowSlotUsage;
	}

	uint32_t GetHighImportanceCount()
	{
		return s_highImportanceLightCount;
	}

	const std::vector<ShadowSlotInfo>& GetSlotInfos()
	{
		return s_shadowSlotInfos;
	}

	const char* GetShadowTypeName(uint32_t type)
	{
		return kShadowTypeNames[std::min(type, 2u)];
	}
}

#undef I18N_KEY_PREFIX
