#include "Features/RemoteControl/DevBenchBridge.h"
#include "Features/Screenshot/BurstAdapter.h"
#include "Features/Screenshot/SequenceContract.h"

#include <algorithm>
#include <cmath>

#include "Utils/FileSystem.h"

// Registers our tools into the devbench test bench over its C-ABI. Gated by
// DEVBENCH_BRIDGE_ENABLED (set by CMake when the devbench-api port is available);
// otherwise this file compiles to an empty Install(). Inert at runtime when no
// devbench plugin is present (GetDevBenchInterface001() returns null).
//
// The openshaders.* tools below expose Open Shaders' graphics-feature, inspect,
// capture, nvperf, shadercache, and settings operations through the single devbench
// host over both MCP and REST. Each is namespaced to avoid collisions in devbench's
// shared registry; the action / kind / inputSchema shapes are stable so clients can
// rely on them.

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "CSEditor/EditorWindow.h"
#	include "Feature.h"
#	include "FeatureIssues.h"
#	include "Features/LightLimitFix/ShadowCasterManager.h"
#	include "Features/RemoteControl/NvPerf.h"
#	include "Features/RemoteControl/ToolSpecs/ActionsTool.h"
#	include "Features/RemoteControl/ToolSpecs/CaptureNativeExt.h"
#	include "Features/RemoteControl/ToolSpecs/CaptureProviderExt.h"
#	include "Features/RemoteControl/ToolSpecs/CaptureTool.h"
#	include "Features/RemoteControl/ToolSpecs/FeatureTool.h"
#	include "Features/RemoteControl/ToolSpecs/InspectFeatureIssuesExt.h"
#	include "Features/RemoteControl/ToolSpecs/InspectLlfShadowsExt.h"
#	include "Features/RemoteControl/ToolSpecs/InspectProfilerExt.h"
#	include "Features/RemoteControl/ToolSpecs/InspectShadercacheExt.h"
#	include "Features/RemoteControl/ToolSpecs/InspectStateExt.h"
#	include "Features/RemoteControl/ToolSpecs/LightHighlightTool.h"
#	include "Features/RemoteControl/ToolSpecs/MenuExt.h"
#	include "Features/RemoteControl/ToolSpecs/NvPerfTool.h"
#	include "Features/RemoteControl/ToolSpecs/PluginApiTool.h"
#	include "Features/RemoteControl/ToolSpecs/ProfilerTool.h"
#	include "Features/RemoteControl/ToolSpecs/SettingsTool.h"
#	include "Features/RemoteControl/ToolSpecs/ShadercacheTool.h"
#	include "Features/RenderDoc.h"
#	include "Features/ScreenshotFeature.h"
#	include "Globals.h"
#	include "I18n/I18n.h"
#	include "Menu.h"
#	include "Profiler.h"
#	include "SceneSettingsManager.h"
#	include "ShaderCache.h"
#	include "State.h"
#	include "Utils/DevBenchUx.h"
#	include "Utils/SettingsCatalog.h"
#	include "Utils/SettingsDefaults.h"
#	include "Utils/SettingsPatch.h"
#	include "Utils/TracyZones.h"
#	include "VRAPI/CSpluginapi.h"

#	include <DevBenchAPI.h>
#	include <nlohmann/json.hpp>

#	include <algorithm>
#	include <chrono>
#	include <cmath>
#	include <cstring>
#	include <format>
#	include <functional>
#	include <future>
#	include <memory>
#	include <optional>
#	include <stdexcept>
#	include <unordered_map>
#	include <unordered_set>

namespace
{
	using json = nlohmann::json;

	/** Expands {brand} into storage that outlives registration, since devbench may keep the pointer. */
	const char* BrandedDescription(const char* a_template)
	{
		static std::deque<std::string> storage;
		return storage.emplace_back(I18n::ExpandBrand(a_template)).c_str();
	}

	// Main thread only: the only features toggle may switch back on. One off for any other
	// reason was never loaded, or turned itself off during boot, and must stay off.
	std::unordered_set<Feature*> g_toggledOffFeatures;

	// Current render frame, used as a coarse "enqueued at" stamp so callers can poll
	// `inspect kind=openshaders` until frame_count advances past it (i.e. a queued
	// main-thread task has had at least one tick to run). Safe from any thread (atomic load).
	uint EnqueuedFrame()
	{
		return globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
	}

	// Exception text can hold ANSI-code-page bytes; a strict dump would throw past the C ABI.
	std::string DumpForDevBench(const json& a_json)
	{
		return a_json.dump(-1, ' ', false, json::error_handler_t::replace);
	}

	// Shared C-ABI handler body. The whole request — parse, dispatch, dump — is wrapped
	// so NO exception ever crosses the DLL boundary, and a_write is called exactly once.
	// `a_build` is a plain function pointer (no captures) so this composes with the
	// captureless-handler contract devbench requires. JSON strings only across the ABI.
	void RunHandler(json (*a_build)(const json&), const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			json args = json::object();
			if (a_argsJson && *a_argsJson)
				args = json::parse(a_argsJson);  // throws on malformed input
			if (!args.is_object())
				throw std::runtime_error("arguments must be a JSON object");
			out = a_build(args);
		} catch (const std::exception& e) {
			out = json{ { "error", "invalid request" }, { "detail", e.what() } };
		} catch (...) {
			out = json{ { "error", "unknown handler error" } };
		}
		const std::string dumped = DumpForDevBench(out);
		a_write(a_sink, dumped.c_str());
	}

	// Run a task on the main thread and return its result. Bridge handlers run on devbench's
	// listener thread; work that touches state the render/UI thread mutates (A/B fields, JSON
	// snapshots, feature settings, capture triggers) must marshal or it races. Blocks the
	// handler briefly, bounded so a stalled main thread (e.g. mid-load) can't hang it.
	//
	// The body may have SIDE EFFECTS (set/reset apply settings; capture triggers a frame), so a
	// `cancelled` flag is checked at task entry: if we already gave up waiting, the task skips
	// the body rather than mutating state after we reported a timeout. shared_ptr state so a
	// task that runs after we return doesn't dangle. (Best-effort: a task that starts exactly at
	// the timeout boundary can still run — the flag eliminates the common stalled-thread case.)
	json RunOnMainThread(std::function<json()> a_run)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		auto prom = std::make_shared<std::promise<json>>();
		auto cancelled = std::make_shared<std::atomic<bool>>(false);
		auto fut = prom->get_future();
		task->AddTask([prom, cancelled, run = std::move(a_run)]() {
			if (cancelled->load(std::memory_order_acquire))
				return;  // handler already timed out and returned — don't run a side-effecting body late
			try {
				prom->set_value(run());
			} catch (const std::exception& e) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", e.what() } });
			} catch (...) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", "non-std exception" } });
			}
		});
		if (fut.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready) {
			cancelled->store(true, std::memory_order_release);
			return json{ { "error", "main thread did not run within 5000ms (mid-load?)" } };
		}
		return fut.get();
	}

	// ---- feature: list / get / set / reset / toggle -----------------------------------

	// Build one feature entry, including restart-gated metadata so `list` answers
	// "what exists", "which fields need a restart", and "is anything pending" in one read.
	json FeatureEntry(Feature* f)
	{
		json entry{
			{ "name", f->GetDisplayName() },
			{ "shortName", f->GetShortName() },
			{ "loaded", f->loaded },
			{ "version", f->version },
			{ "category", std::string(f->GetCategory()) },
			{ "isCore", f->IsCore() },
			{ "supportsVR", f->SupportsVR() },
			{ "inMenu", f->IsInMenu() },
			{ "favorite", globals::state->IsFeatureFavorite(f->GetShortName()) },
			{ "enabledAtBoot", !globals::state->IsFeatureDisabled(f->GetShortName()) },
		};

		const auto fields = f->GetRestartRequiredFields();
		if (!fields.empty()) {
			json restartFields = json::array();
			const auto* liveBase = reinterpret_cast<const unsigned char*>(f->GetSettingsBlob());
			const size_t liveSize = f->GetSettingsBlobSize();
			for (const auto& field : fields) {
				bool pending = false;
				// Compare against remaining bytes (not offset+size, which can overflow and turn
				// a bad metadata entry into an out-of-bounds memcmp).
				if (liveBase && field.jsonKey && field.size != 0 &&
					field.offset <= liveSize && field.size <= liveSize - field.offset) {
					const void* boot = f->GetBootValue(field.jsonKey);
					if (boot && std::memcmp(boot, liveBase + field.offset, field.size) != 0)
						pending = true;
				}
				restartFields.push_back(json{
					{ "key", field.jsonKey ? field.jsonKey : "" },
					{ "label", field.label ? field.label : "" },
					{ "pending", pending },
				});
			}
			entry["restartFields"] = restartFields;
		}
		return entry;
	}

	json BuildFeatureResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string("list"));

		if (action == "list") {
			// Marshal: FeatureEntry reads Feature::loaded and restart-gated settings bytes that
			// main-thread toggles / settings-loads mutate.
			return RunOnMainThread([]() {
				json out = json::array();
				for (auto* f : Feature::GetFeatureList())
					out.push_back(FeatureEntry(f));
				return out;
			});
		}

		const std::string shortName = a_args.value("shortName", std::string{});

		if (action == "favorite" || action == "boot") {
			if (!a_args.contains("enabled") || !a_args["enabled"].is_boolean())
				return json{ { "error", "missing required boolean parameter 'enabled'" } };
			const bool enabled = a_args["enabled"].get<bool>();
			return RunOnMainThread([shortName, action, enabled]() -> json {
				auto& features = Feature::GetFeatureList();
				const auto target = std::ranges::find_if(features, [&shortName](Feature* feature) { return feature->GetShortName() == shortName; });
				if (target == features.end())
					return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
				if (action == "favorite" && (!(*target)->loaded || !(*target)->IsInMenu()))
					return json{ { "error", "favorites require a loaded menu feature" }, { "shortName", shortName } };
				const bool saved = action == "favorite" ? globals::state->SetFeatureFavorite(shortName, enabled) : globals::state->SetFeatureBootEnabled(shortName, enabled);
				if (!saved)
					return json{ { "error", "could not save feature preference" }, { "shortName", shortName } };
				return json{ { "action", action }, { "shortName", shortName }, { "saved", true },
					{ "favorite", globals::state->IsFeatureFavorite(shortName) }, { "enabledAtBoot", !globals::state->IsFeatureDisabled(shortName) } };
			});
		}

		if (action == "toggle") {
			// Match over the full feature list (NOT FindFeatureByShortName, which only
			// matches *loaded* features — that makes toggle one-way: you could disable a
			// feature but never re-enable it).
			Feature* target = nullptr;
			if (!shortName.empty()) {
				for (auto* f : Feature::GetFeatureList()) {
					if (f->GetShortName() == shortName) {
						target = f;
						break;
					}
				}
			}
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				return json{ { "error", "SKSE task interface unavailable" }, { "shortName", shortName } };
			const uint frame = EnqueuedFrame();
			// If `enabled` is omitted we flip the CURRENT value — but that read must happen on
			// the main thread INSIDE the task: computing !target->loaded here on the listener
			// thread lets concurrent toggles all observe the same stale value and enqueue
			// identical results. With an explicit `enabled`, apply it verbatim.
			// Threading contract: Feature::loaded is a public flag the render pipeline reads
			// per-frame via ForEachLoadedFeature without synchronization, hot-toggled by direct
			// assignment — touch it ONLY on the main thread. The applied value is reported via
			// the openshaders.feature.changed event (authoritative; the response can't know an
			// implicit flip's result synchronously).
			const bool hasExplicit = a_args.contains("enabled");
			const bool explicitVal = a_args.value("enabled", false);
			task->AddTask([target, hasExplicit, explicitVal, shortName]() {
				const bool applied = hasExplicit ? explicitVal : !target->loaded;
				if (applied && !target->loaded && !g_toggledOffFeatures.contains(target)) {
					if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
						const std::string payload = json{ { "shortName", shortName }, { "error", "feature was not loaded at boot; enable it at boot and restart" } }.dump();
						dvb->EmitEvent("openshaders.feature.changed", payload.c_str());
					}
					logger::warn("DevBenchBridge: refused to enable '{}' -- not loaded at boot, so its resources were never set up", shortName);
					return;
				}
				// Reject enabling a feature unsupported on the current runtime, unless Developer
				// Mode is on (this repo's existing override escape hatch).
				const bool wantsUnsupportedRuntime = applied && globals::game::isVR && !target->SupportsVR();
				const bool devOverride = globals::state && globals::state->IsDeveloperMode();
				if (wantsUnsupportedRuntime && !devOverride) {
					if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
						const std::string payload = json{ { "shortName", shortName }, { "error", "feature does not support the current runtime; enable rejected" } }.dump();
						dvb->EmitEvent("openshaders.feature.changed", payload.c_str());
					}
					logger::warn("DevBenchBridge: refused to enable '{}' -- unsupported on the current runtime", shortName);
					return;
				}
				if (wantsUnsupportedRuntime)
					logger::warn("DevBenchBridge: enabling '{}' via Developer Mode override despite being unsupported on the current runtime", shortName);
				if (target->loaded && !applied) {
					target->OnRuntimeDisabled();
					g_toggledOffFeatures.insert(target);
				}
				target->loaded = applied;
				if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
					const std::string payload = json{ { "shortName", shortName }, { "enabled", applied } }.dump();
					dvb->EmitEvent("openshaders.feature.changed", payload.c_str());
				}
			});
			json r{ { "action", "toggle" }, { "shortName", shortName }, { "queued", true }, { "enqueued_at_frame", frame } };
			if (hasExplicit)
				r["requested"] = explicitVal;  // implicit flip's result arrives via the event
			return r;
		}

		if (shortName.empty())
			return json{ { "error", "missing required parameter 'shortName'" }, { "action", action } };

		// get / set / reset operate on a loaded feature. FindFeatureByShortName filters on
		// Feature::loaded, which queued toggle tasks mutate on the main thread — so resolve the
		// target INSIDE the main-thread path for each action, never on the listener thread.

		if (action == "get") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				json blob;
				feature->SaveSettings(blob);
				return blob;
			});
		}

		// Live runtime stats (not persisted settings) via Feature::GetDiagnostics;
		// see LightLimitFix for the reference override. Empty object if unimplemented.
		if (action == "diagnostics") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				return feature->GetDiagnostics();
			});
		}

		// Live runtime-only debug flags (never persisted to SettingsUser.json)
		// via Feature::GetRuntimeFlags/SetRuntimeFlag; see LightLimitFix for
		// the reference override. Empty object / false if unimplemented.
		if (action == "runtimeGet") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				return feature->GetRuntimeFlags();
			});
		}

		if (action == "runtimeSet") {
			if (!a_args.contains("name") || !a_args["name"].is_string())
				return json{ { "error", "missing required string parameter 'name'" } };
			if (!a_args.contains("value") || !a_args["value"].is_boolean())
				return json{ { "error", "missing required boolean parameter 'value'" } };
			std::string flagName = a_args["name"];
			bool flagValue = a_args["value"];
			return RunOnMainThread([shortName, flagName, flagValue]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				if (!feature->SetRuntimeFlag(flagName, flagValue))
					return json{
						{ "error", "unrecognized runtime flag" },
						{ "shortName", shortName },
						{ "name", flagName },
						{ "hint", "must match a key from feature(action='runtimeGet')" }
					};
				logger::info("DevBenchBridge: feature(runtimeSet, {}, {}) applied", shortName, flagName);
				return json{ { "action", "runtimeSet" }, { "shortName", shortName }, { "name", flagName }, { "applied", true } };
			});
		}

		// set / reset resolve + apply on the main thread and report the real outcome: an invalid
		// shortName must NOT come back as a fake success. Synchronous (LoadSettings is fast), so
		// the lookup race is avoided AND the caller learns whether the mutation actually applied.
		if (action == "set") {
			if (!a_args.contains("settings") || !a_args["settings"].is_object())
				return json{ { "error", "missing required object parameter 'settings'" } };
			json blob = a_args["settings"];
			return RunOnMainThread([blob, shortName]() mutable -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				try {
					SceneSettingsManager::SceneLayerGuard sceneLayerGuard(
						*SceneSettingsManager::GetSingleton());
					std::vector<std::string> unknown;
					if (!Util::Settings::ApplyPatch(*feature, blob, unknown))
						return json{
							{ "error", "unrecognized setting key(s); nothing applied" },
							{ "shortName", shortName },
							{ "unknownKeys", unknown },
							{ "hint", "keys must match the shape from feature(action='get'); nested groups such as ShadowSettings must be nested, not flattened" }
						};
					logger::info("DevBenchBridge: feature(set, {}) applied", shortName);
					return json{ { "action", "set" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "LoadSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		if (action == "reset") {
			const std::string settingPath = a_args.value("path", std::string{});
			const std::string settingKey = a_args.value("key", std::string{});
			if (!settingPath.empty() && settingKey.empty())
				return json{ { "error", "path requires key" }, { "shortName", shortName }, { "path", settingPath } };
			return RunOnMainThread([shortName, settingPath, settingKey]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				try {
					SceneSettingsManager::SceneLayerGuard sceneLayerGuard(
						*SceneSettingsManager::GetSingleton());
					if (!settingKey.empty()) {
						const auto* setting = SceneSettingsCatalog::FindSetting(shortName, settingPath, settingKey);
						if (!setting || !Util::Settings::ResetSettingToDefault(*feature, *setting))
							return json{ { "error", "setting not found or has no default" }, { "shortName", shortName }, { "path", settingPath }, { "key", settingKey } };
						logger::info("DevBenchBridge: feature(reset, {}, {}/{}) applied", shortName, settingPath, settingKey);
						return json{ { "action", "reset" }, { "shortName", shortName }, { "key", settingKey }, { "applied", true } };
					}
					feature->RestoreDefaultSettings();
					logger::info("DevBenchBridge: feature(reset, {}) applied", shortName);
					return json{ { "action", "reset" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "RestoreDefaultSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		return json{ { "error", "unknown action (list|get|set|reset|toggle|diagnostics|runtimeGet|runtimeSet)" }, { "action", action } };
	}

	void FeatureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildFeatureResult, a_argsJson, a_sink, a_write);
	}

	// ---- actions: devbench-registered UX commands / queries ----------------------------
	// Dispatches by name against Util::DevBenchUx::Registry (see Utils/DevBenchUx.h).
	// Named "actions", not "menu" -- devbench already has a `menu` tool (page navigation
	// via the CommunityShaders extension below); this is unrelated to that.

	bool ParsePerfProfile(const std::string& a_name, Feature::PerfProfile& a_out)
	{
		if (a_name == "performance") {
			a_out = Feature::PerfProfile::Performance;
		} else if (a_name == "balanced") {
			a_out = Feature::PerfProfile::Balanced;
		} else if (a_name == "quality") {
			a_out = Feature::PerfProfile::Quality;
		} else {
			return false;
		}
		return true;
	}

	// Registered for every feature (see RegisterDevBenchUx) -- no per-feature opt-in needed.
	json QueryMatchesPerformanceProfile(const Feature* a_feature, const json& a_args)
	{
		Feature::PerfProfile profile;
		if (!ParsePerfProfile(a_args.value("profile", std::string{}), profile))
			return json{ { "error", "unknown profile (performance|balanced|quality)" } };
		return json{ { "matches", a_feature->MatchesPerformanceProfile(profile) } };
	}

	json QueryGetProfilePreviewText(const Feature* a_feature, const json& a_args)
	{
		Feature::PerfProfile profile;
		if (!ParsePerfProfile(a_args.value("profile", std::string{}), profile))
			return json{ { "error", "unknown profile (performance|balanced|quality)" } };
		return json{ { "text", a_feature->GetProfilePreviewText(profile) } };
	}

	// Called once from Install(): registers the two universal queries above for every known
	// feature, then lets each feature register its own commands/queries via the virtual.
	void RegisterDevBenchUx()
	{
		auto& registry = Util::DevBenchUx::Registry::GetSingleton();
		for (auto* f : Feature::GetFeatureList()) {
			registry.RegisterQuery(f->GetShortName(), "matchesPerformanceProfile",
				"True when this feature's current settings already equal what ApplyPerformanceProfile(profile) would set. Params: profile (performance|balanced|quality).",
				&QueryMatchesPerformanceProfile);
			registry.RegisterQuery(f->GetShortName(), "profilePreviewText",
				"The tooltip text ApplyPerformanceProfile(profile) would show for this feature's profile button. Params: profile (performance|balanced|quality).",
				&QueryGetProfilePreviewText);
			f->RegisterUxActions();
		}
	}

	json BuildActionsResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		const std::string shortName = a_args.value("shortName", std::string{});

		if (action == "listCommands" || action == "listQueries") {
			if (!Feature::FindRegisteredFeatureByShortName(shortName))
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			auto& registry = Util::DevBenchUx::Registry::GetSingleton();
			const bool isCommands = action == "listCommands";
			return json{
				{ "shortName", shortName },
				{ isCommands ? "commands" : "queries", isCommands ? registry.ListCommands(shortName) : registry.ListQueries(shortName) },
			};
		}

		const std::string name = a_args.value("name", std::string{});
		const json actionArgs = a_args.value("args", json::object());

		if (action == "invokeCommand") {
			const auto* entry = Util::DevBenchUx::Registry::GetSingleton().FindCommand(shortName, name);
			if (!entry)
				return json{ { "error", "unknown command" }, { "shortName", shortName }, { "name", name } };
			Feature* target = Feature::FindRegisteredFeatureByShortName(shortName);
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				return json{ { "error", "SKSE task interface unavailable" } };
			const uint frame = EnqueuedFrame();
			const auto fn = entry->fn;
			task->AddTask([target, fn, actionArgs, shortName, name]() {
				try {
					fn(target, actionArgs);
					logger::info("DevBenchBridge: menu(invokeCommand, {}.{}) ran", shortName, name);
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: menu(invokeCommand, {}.{}) threw: {}", shortName, name, e.what());
				} catch (...) {
					logger::error("DevBenchBridge: menu(invokeCommand, {}.{}) threw (unknown)", shortName, name);
				}
			});
			return json{ { "action", "invokeCommand" }, { "shortName", shortName }, { "name", name }, { "queued", true }, { "enqueued_at_frame", frame } };
		}

		if (action == "invokeQuery") {
			const auto* entry = Util::DevBenchUx::Registry::GetSingleton().FindQuery(shortName, name);
			if (!entry)
				return json{ { "error", "unknown query" }, { "shortName", shortName }, { "name", name } };
			Feature* target = Feature::FindRegisteredFeatureByShortName(shortName);
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			// Marshal: query implementations read live feature settings the render/UI thread mutates.
			const auto fn = entry->fn;
			return RunOnMainThread([target, fn, actionArgs]() {
				return fn(target, actionArgs);
			});
		}

		return json{ { "error", "unknown action (listCommands|listQueries|invokeCommand|invokeQuery)" }, { "action", action } };
	}

	void ActionsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildActionsResult, a_argsJson, a_sink, a_write);
	}

	// ---- inspect: engine state / shader-cache status ----------------------------------

	// Registered as base-tool extensions on devbench 1.5.0+ (`inspect kind=openshaders`,
	// `inspect kind=shadercache`); the base tool routes by key, so each handler ignores
	// the kind arg and returns its own object.
	json BuildInspectStateResult(const json&)
	{
		return json{
			{ "plugin", "CommunityShaders" },
			{ "frame_count", EnqueuedFrame() },
			{ "vr", globals::game::isVR },
		};
	}

	json MismatchesToJson(const std::vector<Util::CacheInvalidation::CacheMismatch>& a_mismatches)
	{
		json out = json::array();
		for (const auto& mismatch : a_mismatches) {
			out.push_back(json{
				{ "kind", magic_enum::enum_name(mismatch.kind) },
				{ "shortName", mismatch.shortName },
				{ "feature", mismatch.feature },
				{ "detail", mismatch.detail },
				{ "nowPresent", mismatch.nowPresent },
			});
		}
		return out;
	}

	json RecentFailuresToJson(const std::vector<SIE::ShaderCache::CompileFailure>& a_failures)
	{
		json out = json::array();
		for (const auto& f : a_failures) {
			out.push_back(json{
				{ "key", f.key },
				{ "path", f.path },
				{ "error", f.error },
				{ "epoch", f.epoch },
				{ "frame", f.frame },
			});
		}
		return out;
	}

	json BuildInspectShadercacheResult(const json&)
	{
		// Built from thread-safe ShaderCache accessors. Poll completedTasks against a
		// pre-deploy snapshot to know a hot-reloaded shader finished; a rising
		// failedTasks / currentFailedCount surfaces an otherwise-invisible failed compile;
		// recentFailures identifies WHICH shader (source file + feature defines) and why.
		auto* cache = globals::shaderCache;
		if (!cache)
			return json{ { "error", "shader cache unavailable" } };
		const auto rollback = cache->GetRollbackState();
		return json{
			{ "compiling", cache->IsCompiling() },
			{ "completedTasks", cache->GetCompletedTasks() },
			{ "totalTasks", cache->GetTotalTasks() },
			{ "failedTasks", cache->GetFailedTasks() },
			{ "currentFailedCount", cache->GetCurrentFailedCount() },
			{ "recentFailures", RecentFailuresToJson(cache->GetRecentCompileFailures()) },
			{ "diskHitTasks", cache->GetDiskHitTasks() },
			{ "digestHitTasks", cache->GetDigestHitTasks() },
			{ "contentStoreHitTasks", cache->GetContentStoreHitTasks() },
			{ "contentDedupeTasks", cache->GetContentDedupeTasks() },
			{ "activeReuseTasks", cache->GetActiveReuseTasks() },
			{ "previousReuseTasks", cache->GetPreviousReuseTasks() },
			{ "digestMissTasks", cache->GetDigestMissTasks() },
			{ "digestComputeCount", cache->GetDigestComputeCount() },
			{ "digestComputeTimeUs", cache->GetDigestComputeTimeUs() },
			{ "frame_count", EnqueuedFrame() },
			// Rollback/backup-slot state, mirrored by the restorePrevious/acceptRebuild actions.
			{ "diskCacheHeld", rollback.diskCacheHeld },
			{ "featureSetChanged", rollback.featureSetChanged },
			{ "featureSetRevertPending", rollback.featureSetRevertPending },
			{ "featureSetCacheBackedUp", rollback.featureSetCacheBackedUp },
			{ "previousDiskCacheAvailable", rollback.previousDiskCacheAvailable },
			{ "cacheMismatches", MismatchesToJson(rollback.cacheMismatches) },
			{ "previousCacheMismatches", MismatchesToJson(rollback.previousCacheMismatches) },
		};
	}

	json FeatureIssueToJson(const FeatureIssues::FeatureIssueInfo& a_issue)
	{
		return json{
			{ "shortName", a_issue.shortName },
			{ "displayName", a_issue.displayName },
			{ "version", a_issue.version },
			{ "issueType", magic_enum::enum_name(a_issue.issueType) },
			{ "rejectionReason", a_issue.rejectionReason },
			{ "replacementFeature", a_issue.replacementFeature },
			{ "replacementFeatureDisplayName", a_issue.replacementFeatureDisplayName },
			{ "replacementFeatureInstalled", a_issue.replacementFeatureInstalled },
			{ "replacementFeatureModLink", a_issue.replacementFeatureModLink },
			{ "userMessage", a_issue.userMessage },
			{ "minimumVersionRequired", a_issue.minimumVersionRequired },
			{ "modifiedShaderDirectory", a_issue.modifiedShaderDirectory },
			{ "iniPath", a_issue.iniPath },
			{ "removedInVersion", a_issue.removedInVersion.string(".") },
		};
	}

	// FeatureIssues' backing vector is main-thread-owned (populated at boot and by the
	// in-menu refresh action) with no lock of its own -- marshal onto the main thread
	// like every other main-thread-state read in this file, rather than racing it.
	json BuildInspectFeatureIssuesResult(const json&)
	{
		return RunOnMainThread([]() -> json {
			json issues = json::array();
			for (const auto& issue : FeatureIssues::GetFeatureIssues())
				issues.push_back(FeatureIssueToJson(issue));
			return json{
				{ "featureIssues", issues },
				{ "hasIssues", FeatureIssues::HasFeatureIssues() },
				{ "hasObsoleteShaderModifyingFeatures", FeatureIssues::HasObsoleteShaderModifyingFeatures() },
				{ "hasPotentialShaderModifyingFeatures", FeatureIssues::HasPotentialShaderModifyingFeatures() },
			};
		});
	}

	void InspectFeatureIssuesHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectFeatureIssuesResult, a_argsJson, a_sink, a_write);
	}

	// Light Limit Fix shadow-scheduler diagnostics. RequestSchedSnapshot is thread-safe
	// (snapshot copied under a mutex by the render thread) and primes the scheduler to
	// keep filling it for a short window, so a first idle call may return valid=false --
	// poll again after a frame. Light pointers are hex strings (JSON-number-safe).
	json BuildInspectShadowsResult(const json&)
	{
		const auto snap = ShadowCasterManager::RequestSchedSnapshot();
		json lights = json::array();
		for (const auto& [ptr, reason] : snap.demoted)
			lights.push_back(json{
				{ "ptr", std::format("{:#018x}", ptr) },
				{ "reason", ShadowCasterManager::SchedReasonName(reason) },
			});
		json visibility = json::array();
		int visEngineOutOursIn = 0;
		int visEngineInOursOut = 0;
		for (const auto& v : snap.visibility) {
			visEngineOutOursIn += (v.engineCulled && v.ownInFrustum) ? 1 : 0;
			visEngineInOursOut += (!v.engineCulled && !v.ownInFrustum) ? 1 : 0;
			visibility.push_back(json{
				{ "ptr", std::format("{:#018x}", v.light) },
				{ "chosen", v.chosen },
				{ "engineCulled", v.engineCulled },
				{ "frustrumCull", v.frustrumCull },
				{ "lodDimmer", v.lodDimmer },
				{ "distance", v.distance },
				{ "radius", v.radius },
				{ "ownInFrustum", v.ownInFrustum },
				{ "cameraHold", v.cameraHold },
				{ "hidden", v.hidden },
				{ "shadowDistanceCulled", v.shadowDistanceCulled },
				{ "frustumCulled", v.frustumCulled },
				{ "name", v.name },
				{ "x", v.x },
				{ "y", v.y },
				{ "z", v.z },
			});
		}
		json slots = json::array();
		// Bucketed from tile.size, not renderedScale, which reflects the requested
		// scale -- a starved light can request full and get only the smallest tile.
		int classes[5] = {};
		int classNone = 0;  // tileSize == 0: no atlas tile, distinct from the smallest class
		const float baseTexels = snap.baseTileTexels > 0.0f ? snap.baseTileTexels : 2048.0f;
		for (const auto& s : snap.slots) {
			if (s.tileSize == 0) {
				classNone++;
			} else {
				int cls = 0;
				const float tileScale = static_cast<float>(s.tileSize) / baseTexels;
				for (float step = 1.0f; tileScale < step && cls < 4; step *= 0.5f)
					cls++;
				classes[cls]++;
			}
			slots.push_back(json{
				{ "slot", s.index },
				{ "ptr", std::format("{:#018x}", s.light) },
				{ "importance", s.importance },
				{ "score", s.score },
				{ "desiredScale", s.desiredScale },
				{ "budgetScale", s.budgetScale },
				{ "pendingScale", s.pendingScale },
				{ "renderedScale", s.renderedScale },
				{ "demandMax", s.demandMax },
				{ "untouchedSamples", s.untouchedSamples },
				{ "litMax", s.litMax },
				{ "litReason", s.litReason },
				{ "litTile", s.litTile },
				{ "litSum", s.litSum },
				{ "litSmoothed", s.litSmoothed },
				{ "litCapped", s.litCapped },
				{ "tile", json{ { "x", s.tileX }, { "y", s.tileY }, { "size", s.tileSize }, { "contentValid", s.tileContentValid } } },
				{ "geomListSize", s.geomListSize },
				{ "staticValid", s.staticValid },
				{ "staticEmpty", s.staticEmpty },
				{ "upload", json{ { "recorded", s.uploadRecorded }, { "paramY", s.uploadParamY }, { "range", s.uploadRange } } },
				{ "suppressed", s.suppressed },
				{ "promoted", s.promoted },
				{ "redrawnThisFrame", s.redrawnThisFrame },
				{ "schedDirty", s.schedDirty },
				{ "dirtyStallFrames", s.dirtyStallFrames },
				{ "redrawScore", s.redrawScore },
				{ "lastDrawnFrame", s.lastDrawnFrame },
				{ "cameraHold", s.cameraHold },
			});
		}
		return json{
			{ "valid", snap.valid },
			{ "frame", snap.frame },
			{ "total", snap.total },
			{ "chosen", snap.chosen },
			{ "excess", snap.excess },
			{ "invalidCamera", snap.invalidCamera },
			{ "invalidPortal", snap.invalidPortal },
			{ "invalidFrustum", snap.invalidFrustum },
			{ "invalidLod", snap.invalidLod },
			{ "invalidOther", snap.invalidOther },
			{ "slotsInUse", snap.slotsInUse },
			{ "lights", lights },
			{ "visibility", visibility },
			{ "visEngineOutOursIn", visEngineOutOursIn },
			{ "visEngineInOursOut", visEngineInOursOut },
			{ "slots", slots },
			{ "classes", json{ { "full", classes[0] }, { "half", classes[1] }, { "quarter", classes[2] }, { "eighth", classes[3] }, { "sixteenth", classes[4] }, { "none", classNone } } },
			{ "atlas", json{
						   { "dim", snap.atlasDim },
						   { "baseTileTexels", snap.baseTileTexels },
						   { "capacityCells", snap.atlasCapacityCells },
						   { "occupancy", snap.atlasOccupancy },
						   { "vramBytes", snap.atlasVramBytes },
						   { "tileReallocs", snap.atlasTileReallocs },
						   { "ownerInvalidations", snap.atlasOwnerInvalidations },
						   { "allocDenied", snap.atlasAllocDenied },
						   { "cpuAccumUsAvg", snap.cpuAccumUsAvg },
						   { "cpuSubmitUsAvg", snap.cpuSubmitUsAvg },
						   { "cpuEnableUsAvg", snap.cpuEnableUsAvg },
					   } },
			{ "budget", json{
							{ "avgLightCostUs", snap.avgLightCostUs },
							{ "avgRedrawsPerFrame", snap.avgRedrawsPerFrame },
							{ "estPassMsPerFrame", snap.avgLightCostUs / 1000.0 * snap.avgRedrawsPerFrame },
							{ "staticBakesTotal", snap.staticBakesTotal },
							{ "passGuardChecksTotal", snap.passGuardChecksTotal },
							{ "passGuardCycleSkipsTotal", snap.passGuardCycleSkipsTotal },
							{ "passGuardFaultSkipsTotal", snap.passGuardFaultSkipsTotal },
							{ "passGuardCapExceededTotal", snap.passGuardCapExceededTotal },
							{ "passGuardCycleRepairsTotal", snap.passGuardCycleRepairsTotal },
							{ "staleAccumulatesTotal", snap.staleAccumulatesTotal },
							{ "staleAfterRenderSkipTotal", snap.staleAfterRenderSkipTotal },
							{ "stalePassClearsTotal", snap.stalePassClearsTotal },
							{ "renderSkipSessionResetTotal", snap.renderSkipsByReason[0] },
							{ "renderSkipPortalTransitionTotal", snap.renderSkipsByReason[1] },
							{ "renderSkipTeardownWaitingTotal", snap.renderSkipsByReason[2] },
							{ "renderSkipTeardownRaceTotal", snap.renderSkipsByReason[3] },
							{ "passRegChecksTotal", snap.passRegChecksTotal },
							{ "passRegRingsTotal", snap.passRegRingsTotal },
							{ "stalePromotedTotal", snap.stalePromotedTotal },
							{ "passGuardRepairsPromotedTotal", snap.passGuardRepairsPromotedTotal },
							{ "passRegRingsPromotedTotal", snap.passRegRingsPromotedTotal },
							{ "splitAccumAllTotal", snap.splitAccumByMode[0] },
							{ "splitAccumStaticOnlyTotal", snap.splitAccumByMode[1] },
							{ "splitAccumDynamicOnlyTotal", snap.splitAccumByMode[2] },
							{ "splitLatchMismatchTotal", snap.splitLatchMismatchTotal },
							{ "splitLatchWindowTotal", snap.splitLatchWindowTotal },
							{ "splitLatchRetryTotal", snap.splitLatchRetryTotal },
							{ "splitWastedBakesTotal", snap.splitWastedBakesTotal },
							{ "scalePromotionRedraws", snap.scalePromotionRedraws },
							{ "scaleDemotionRedrawsAvoided", snap.scaleDemotionRedrawsAvoided },
							{ "scalePromotionRedundantAvoided", snap.scalePromotionRedundantAvoided },
							{ "convertReEnableRun", snap.convertReEnableRun },
							{ "convertReEnableSkipped", snap.convertReEnableSkipped },
							{ "convertReEnableToleranceSkipped", snap.convertReEnableToleranceSkipped },
							{ "originPinnedLights", snap.originPinnedLights },
							{ "warpsRun", snap.warpsRun },
							{ "warpsSkippedSubPixel", snap.warpsSkippedSubPixel },
							{ "warpsSkippedOffscreen", snap.warpsSkippedOffscreen },
							{ "warpsSkippedNoDemand", snap.warpsSkippedNoDemand },
							{ "convertedLights", snap.convertedLights },
							{ "convertedStrictLights", snap.convertedStrictLights },
							{ "cellResetsTotal", snap.cellResetsTotal },
							{ "cullPoolDropsTotal", snap.cullPoolDropsTotal },
							{ "casterCullDropsTotal", snap.casterCullDropsTotal },
							{ "hemisphereReassignTotal", snap.hemisphereReassignTotal },
							{ "frontAppendTotal", snap.frontAppendTotal },
							{ "backOnlyAppendTotal", snap.backOnlyAppendTotal },
							{ "hemisphereDropTotal", snap.hemisphereDropTotal },
							{ "passlessCasterSkipTotal", snap.passlessCasterSkipTotal },
							{ "passlessSkipNoShadowShader", snap.passlessCasterSkipByReason[1] },
							{ "passlessSkipRefraction", snap.passlessCasterSkipByReason[2] },
							{ "passlessSkipAlphaBlend", snap.passlessCasterSkipByReason[3] },
							{ "passlessSkipFaded", snap.passlessCasterSkipByReason[4] },
							{ "sleepSkips", snap.sleepSkips },
							{ "sleepSkipsTotal", snap.sleepSkipsTotal },
							{ "demandSkips", snap.demandSkips },
							{ "demandSkipsTotal", snap.demandSkipsTotal },
							{ "playerProtected", snap.playerProtected },
							{ "playerProtectedTotal", snap.playerProtectedTotal },
							{ "playerForcedPastBudget", snap.playerForcedPastBudget },
							{ "playerForcedPastBudgetTotal", snap.playerForcedPastBudgetTotal },
							{ "playerProtectedExcess", snap.playerProtectedExcess },
							{ "playerProtectedExcessTotal", snap.playerProtectedExcessTotal },
							{ "alphaGroupPeak", snap.alphaGroupPeak },
							{ "alphaGroupDrops", snap.alphaGroupDrops },
						} },
			{ "demandAudit", json{
								 { "frustumCandidates", snap.frustumAuditCandidates },
								 { "frustumKeptSphereOut", snap.frustumAuditKeptOut },
								 { "frustumSuspects", snap.frustumAuditSuspects },
								 { "slotted", snap.demandSlotted },
								 { "zero", snap.demandZero },
								 { "subTap", snap.demandSubTap },
								 { "skipEligible", snap.demandSkipEligible },
								 { "swapIn", snap.demandSwapIn },
								 { "swapInAboveEps", snap.demandSwapInAboveEps },
								 { "redrawsSaved", snap.demandRedrawsSaved },
								 { "budgetSaturated", snap.demandBudgetSaturated },
								 { "phase1Enabled", snap.demandPhase1Enabled },
								 { "skipActive", snap.demandSkipActive },
								 { "skipEligibleTotal", snap.demandSkipEligibleTotal },
								 { "swapInTotal", snap.demandSwapInTotal },
								 { "redrawsSavedTotal", snap.demandRedrawsSavedTotal },
							 } },
			{ "redrawStall", json{
								 { "stallMax", snap.stallMax },
								 { "stallWorstSlot", snap.stallWorstSlot },
								 { "demandRatio", snap.demandRatio },
							 } },
		};
	}

	void InspectStateHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectStateResult, a_argsJson, a_sink, a_write);
	}

	void InspectShadercacheHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectShadercacheResult, a_argsJson, a_sink, a_write);
	}

	void InspectShadowsHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectShadowsResult, a_argsJson, a_sink, a_write);
	}

	// Marshaled to the main thread: GetResults() returns a live reference the
	// render thread clears+rebuilds every frame, and TimerResult's history
	// pointers dangle across a knownTimers reallocation -- nothing from it
	// may be read or retained off that thread.
	json BuildInspectProfilerResult(const json&)
	{
		auto* profiler = globals::profiler;
		if (!profiler)
			return json{ { "error", "profiler unavailable" } };
		return RunOnMainThread([profiler]() -> json {
			// Calling this tool is itself a capture request, the same as any
			// visible consumer (the Profiling menu, a feature panel).
			profiler->RequestCapture();

			json timers = json::array();
			for (const auto& r : profiler->GetStatistics()) {
				if (!r.valid)
					continue;
				timers.push_back(json{
					{ "name", r.name },
					{ "gpuMs", r.gpuTimeMs },
					{ "topLevelMs", r.topLevelMs },
					{ "avgMs", r.avgMs },
					{ "p95Ms", r.p95Ms },
					{ "p99Ms", r.p99Ms },
					{ "cpuMs", r.cpuTimeMs },
					{ "cpuAvgMs", r.cpuAvgMs },
					{ "cpuP95Ms", r.cpuP95Ms },
					{ "cpuP99Ms", r.cpuP99Ms },
					{ "hasGpu", r.hasGpu },
					{ "hasCpu", r.hasCpu },
					{ "activeGpu", r.activeGpu },
					{ "activeCpu", r.activeCpu },
					{ "historyHead", r.historyHead },
					{ "historyCount", r.historyCount },
					{ "cpuHistoryHead", r.cpuHistoryHead },
					{ "cpuHistoryCount", r.cpuHistoryCount },
				});
			}

			return json{
				{ "enabled", profiler->IsUserEnabled() },
				{ "capturing", profiler->IsEnabled() },
				{ "frame_count", EnqueuedFrame() },
				{ "capturedFrameCount", profiler->GetCapturedFrameCount() },
				{ "capturedGpuFrameCount", profiler->GetCapturedGpuFrameCount() },
				{ "capturedCpuFrameCount", profiler->GetCapturedCpuFrameCount() },
				{ "totalMs", profiler->GetTotalTimeMs() },
				{ "cpuTotalMs", profiler->GetCpuTotalTimeMs() },
				{ "resolvedTotalMs", profiler->GetResolvedTotalTimeMs() },
				{ "resolvedCpuTotalMs", profiler->GetResolvedCpuTotalTimeMs() },
				{ "acquiredSlots", profiler->GetAcquiredSlots() },
				{ "peakAcquiredSlots", profiler->GetPeakAcquiredSlots() },
				{ "slotRefusals", profiler->GetSlotRefusals() },
				{ "maxTimers", Profiler::kMaxTimers },
				{ "timers", timers },
			};
		});
	}

	void InspectProfilerHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectProfilerResult, a_argsJson, a_sink, a_write);
	}

	// ---- shadercache: clear / delete the compiled cache -------------------------------

	json BuildShadercacheResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		auto* cache = globals::shaderCache;
		if (!cache)
			return json{ { "error", "shader cache unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// Mutating cache ops touch the live ShaderCache (and, for deleteDisk, the filesystem)
		// — marshal to the main thread. Recompiles are observable via inspect(kind=shadercache)
		// + the openshaders.shaderRecompiled event. NOTE clear vs deleteDisk: clear only drops
		// the in-memory maps, so with the disk cache enabled shaders reload from Data/ShaderCache
		// rather than recompiling — only deleteDisk guarantees a cold recompile.
		if (action == "clear") {
			task->AddTask([cache]() { cache->Clear(); });
			return json{ { "action", "clear" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "in-memory cache dropped; shaders reload from the disk cache if present, else recompile (use deleteDisk to force a cold recompile)" } };
		}
		if (action == "deleteDisk") {
			// Delete on disk AND drop the in-memory cache — otherwise existing variants keep
			// serving from memory and the promised full recompile never happens (mirrors
			// PerformClearShaderCache and the ShaderCache invalidation path).
			task->AddTask([cache]() {
				cache->DeleteDiskCache();
				cache->Clear();
			});
			return json{ { "action", "deleteDisk" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "on-disk + in-memory shader cache cleared; a full recompile follows (cold-compile benchmark)" } };
		}
		if (action == "activeOnly") {
			// Mirrors the in-game "smart clear" button's ActiveOnly scope: only evicts
			// shaders actually on screen across two capture windows, exposed here so that
			// scope (CompilationSet::Forget's re-arm path) is exercisable without the UI.
			task->AddTask([cache]() { cache->BeginActiveShaderCapture(); });
			return json{ { "action", "activeOnly" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "scoped (smart) clear started; captures on-screen shaders over two windows, then evicts+recompiles just those (see inspect(kind=shadercache) and openshaders.shaderRecompiled)" } };
		}
		if (action == "backgroundCompile") {
			// Same atomic the in-game "Skip Compilation" hotkey flips (Menu.cpp);
			// unblocks XSEPlugin.cpp's boot-wait loop on its next check.
			cache->backgroundCompilation = true;
			return json{ { "action", "backgroundCompile" }, { "backgroundCompilation", true } };
		}
		if (action == "contentStore") {
			// Same atomic the Advanced menu checkbox flips; read per compile, so no marshalling needed.
			if (!a_args.contains("enabled") || !a_args["enabled"].is_boolean())
				return json{ { "error", "contentStore requires a boolean 'enabled'" } };
			const bool enabled = a_args["enabled"].get<bool>();
			globals::state->enableContentStore.store(enabled, std::memory_order_relaxed);
			return json{ { "action", "contentStore" }, { "enabled", enabled } };
		}
		if (action == "acceptRebuild") {
			// Mirrors the in-game held-cache prompt's "rebuild" button (HomePageRenderer.cpp):
			// discards the held cache's stale-feature blobs and rebuilds for the current setup.
			task->AddTask([cache]() { cache->AcceptCacheRebuild(); });
			return json{ { "action", "acceptRebuild" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "accepted the feature-set change; disk cache rebuilding for the current setup, see inspect(kind=shadercache)" } };
		}
		if (action == "restorePrevious") {
			// Mirrors the in-game held-cache prompt's "restore previous" button; cancels an
			// in-progress compile if one is running. Only takes effect after a restart.
			task->AddTask([cache]() { cache->RestorePreviousDiskCache(); });
			return json{ { "action", "restorePrevious" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "cancels any in-progress compile and needs a compatible previous cache; check OpenShaders.log or inspect(kind=shadercache).featureSetRevertPending for the outcome, then restart to load it" } };
		}
		if (action == "discardPrevious") {
			// Mirrors the in-game "keep current cache" button: drops the rollback cache so the mismatch notice clears.
			task->AddTask([cache]() { cache->DiscardPreviousDiskCache(); });
			return json{ { "action", "discardPrevious" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "refused while a feature-set change awaits commit or a restore is pending; check inspect(kind=shadercache).featureSetChanged and OpenShaders.log for the outcome" } };
		}
		if (action == "exportTrace") {
			// Only reads records (mutex-guarded) and writes a file -- safe off-thread. Don't
			// add render/UI-state access here without marshalling like the actions above.
			const auto logDir = Util::PathHelpers::GetLogPath().parent_path();
			std::filesystem::path path = logDir / "compile-trace.json";
			if (a_args.contains("path")) {
				const std::filesystem::path requested = Util::PathHelpers::FromUtf8(a_args.value("path", std::string{}));
				if (requested.is_absolute()) {
					return json{ { "action", "exportTrace" }, { "success", false }, { "error", "path must be relative to the log directory, not absolute" } };
				}
				const auto resolved = (logDir / requested).lexically_normal();
				const auto rel = resolved.lexically_relative(logDir);
				const bool escapesLogDir = rel.empty() || std::any_of(rel.begin(), rel.end(), [](const auto& part) { return part == ".."; });
				if (escapesLogDir) {
					return json{ { "action", "exportTrace" }, { "success", false }, { "error", "path escapes the log directory" } };
				}
				path = resolved;
			}
			const bool ok = cache->ExportCompileTrace(path);
			if (!ok) {
				return json{ { "action", "exportTrace" }, { "success", false }, { "error", "export failed or no task records for the current build; see OpenShaders.log" } };
			}
			return json{ { "action", "exportTrace" }, { "success", true }, { "path", Util::PathHelpers::ToUtf8(path) } };
		}
		return json{ { "error", "unknown action (clear|deleteDisk|activeOnly|backgroundCompile|contentStore|acceptRebuild|restorePrevious|discardPrevious|exportTrace)" }, { "action", action } };
	}

	json BuildLightHighlightResult(const json& a_args)
	{
		uintptr_t key = 0;
		if (a_args.contains("ptr") && a_args["ptr"].is_string()) {
			const std::string text = a_args["ptr"].get<std::string>();
			if (text != "none")
				key = static_cast<uintptr_t>(std::stoull(text, nullptr, 16));
		}
		ShadowCasterManager::SetDebugHighlightLight(key);
		return json{ { "ptr", std::format("{:#018x}", key) } };
	}

	void LightHighlightToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildLightHighlightResult, a_argsJson, a_sink, a_write);
	}

	void ShadercacheToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildShadercacheResult, a_argsJson, a_sink, a_write);
	}

	// ---- profiler: enable / disable runtime capture ----------------------------------

	json BuildProfilerResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		auto* profiler = globals::profiler;
		if (!profiler)
			return json{ { "error", "profiler unavailable" } };
		if (action == "enable") {
			return RunOnMainThread([profiler]() -> json {
				profiler->SetUserEnabled(true);
				return json{ { "action", "enable" }, { "enabled", true } };
			});
		}
		if (action == "disable") {
			return RunOnMainThread([profiler]() -> json {
				profiler->SetUserEnabled(false);
				return json{ { "action", "disable" }, { "enabled", false } };
			});
		}
		if (action == "perDrawZones") {
			if (a_args.contains("enabled") && a_args["enabled"].is_boolean())
				TracyZones::SetPerDrawZones(a_args["enabled"].get<bool>());
			return json{
				{ "action", "perDrawZones" },
				{ "enabled", TracyZones::PerDrawZonesEnabled() },
				{ "callsSinceLastFrame", { { "State::Draw", TracyZones::Snapshot(TracyZones::Family::StateDraw) }, { "OnRenderPassBegin", TracyZones::Snapshot(TracyZones::Family::RenderPassHook) } } }
			};
		}
		return json{ { "error", "unknown action (enable|disable|perDrawZones)" }, { "action", action } };
	}

	void ProfilerToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildProfilerResult, a_argsJson, a_sink, a_write);
	}

	// ---- capture: renderdoc / screenshot ----------------------------------------------

	json BuildCaptureResult(const json& a_args)
	{
		const std::string kind = a_args.value("kind", std::string{});
		if (kind.empty())
			return json{ { "error", "missing required parameter 'kind'" } };
		const uint frame = EnqueuedFrame();

		if (kind == "renderdoc") {
			uint32_t frameCount = 1;
			if (a_args.contains("frames") && a_args["frames"].is_number())
				frameCount = static_cast<uint32_t>(std::clamp(a_args["frames"].get<int>(), 1, 120));
			// All on the main thread: Feature::loaded is mutated by queued toggle tasks there,
			// and TriggerCapture invalidates RenderDoc's non-atomic capture-list cache the
			// UI/render thread reads.
			return RunOnMainThread([frameCount, frame]() -> json {
				auto* renderDoc = &globals::features::renderDoc;
				if (!renderDoc->loaded)
					return json{ { "error", "RenderDoc feature is not loaded" }, { "hint", "openshaders.feature(action='list') shows RenderDoc.loaded" } };
				if (!renderDoc->IsAvailable())
					return json{ { "error", "RenderDoc API not available — attach RenderDoc or load the in-app DLL" } };
				if (frameCount == 1)
					renderDoc->TriggerCapture();
				else
					renderDoc->TriggerMultiFrameCapture(frameCount);
				return json{ { "queued", true }, { "kind", "renderdoc" }, { "frames", frameCount }, { "enqueued_at_frame", frame } };
			});
		}

		if (kind == "screenshot") {
			// loaded read on the main thread (toggle tasks mutate it); the request flag is atomic.
			return RunOnMainThread([frame]() -> json {
				auto* shot = &globals::features::screenshotFeature;
				if (!shot->loaded)
					return json{ { "error", "Screenshot feature is not loaded" } };
				shot->captureRequested.store(true, std::memory_order_release);
				return json{ { "queued", true }, { "kind", "screenshot" }, { "enqueued_at_frame", frame } };
			});
		}

		if (kind == "shadowmaps") {
			// Atomic request flags serviced by the render thread's shadow pass;
			// everything lands under CommunityShaders/Captures. Default: one
			// whole-atlas DDS + manifest. With frames=N: a multi-frame recorder
			// (per-slot numeric state per frame; slot>=0 adds that light's
			// caster set per pass mode, and frames<=16 adds per-frame tile DDS).
			const uint32_t frames = a_args.value("frames", 0u);
			if (a_args.contains("slot") && !a_args["slot"].is_number_integer())
				return json{ { "error", "slot must be an integer" } };
			const int32_t slot = a_args.value("slot", -1);
			if (frames > 0) {
				ShadowCasterManager::RequestShadowFrameRecord(frames, slot);
				return json{ { "queued", true }, { "kind", "shadowmaps" }, { "frames", frames },
					{ "slot", slot }, { "enqueued_at_frame", frame },
					{ "note", "recorder armed; scm_frames_<N>.json (and scm_rec_slot<S>_f<N>.dds when slot>=0, frames<=16) under CommunityShaders/Captures at completion; watch the log" } };
			}
			ShadowCasterManager::RequestAtlasDump();
			return json{ { "queued", true }, { "kind", "shadowmaps" }, { "enqueued_at_frame", frame },
				{ "note", "written by the next shadow pass to Data/SKSE/Plugins/CommunityShaders/Captures (atlas mode only); watch the log for the path" } };
		}

		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "renderdoc", "screenshot", "shadowmaps" }) } };
	}

	void CaptureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildCaptureResult, a_argsJson, a_sink, a_write);
	}

	// ---- nvperf: status / metrics / capture / cancel / result ---------------------------

	const char* CaptureStateName(NvPerf::CaptureState a_state)
	{
		switch (a_state) {
		case NvPerf::CaptureState::Waiting:
			return "waiting";
		case NvPerf::CaptureState::Capturing:
			return "capturing";
		case NvPerf::CaptureState::Decoding:
			return "decoding";
		case NvPerf::CaptureState::Done:
			return "done";
		case NvPerf::CaptureState::Failed:
			return "failed";
		default:
			return "idle";
		}
	}

	json StatusJson(const NvPerf::Status& a_status)
	{
		return json{ { "state", CaptureStateName(a_status.state) }, { "captureId", a_status.captureId }, { "sample", a_status.sample },
			{ "samples", a_status.samples }, { "pass", a_status.pass }, { "passes", a_status.passes }, { "error", a_status.error } };
	}

	json MetricJson(const NvPerf::MetricRequest& a_metric)
	{
		return json{ { "metric", NvPerf::FormatMetric(a_metric) }, { "label", NvPerf::MetricLabel(a_metric) } };
	}

	/**
	 * @brief Serializes a capture result: the per-filter summary always, every range of every sample on request.
	 *
	 * Summary values are per frame and keyed by metric label; a value no replay pass completed is null.
	 */
	json CaptureResultJson(const NvPerf::CaptureResult& a_result, bool a_detail)
	{
		json metrics = json::array();
		std::vector<std::string> labels;
		for (const auto& metric : a_result.metrics) {
			metrics.push_back(MetricJson(metric));
			labels.push_back(NvPerf::MetricLabel(metric));
		}
		json unsupported = json::array();
		for (const auto& metric : a_result.unsupportedMetrics)
			unsupported.push_back(MetricJson(metric));
		const auto values = [&](const std::vector<double>& a_values) {
			json out = json::object();
			for (std::size_t m = 0; m < labels.size() && m < a_values.size(); ++m)
				out[labels[m]] = std::isnan(a_values[m]) ? json(nullptr) : json(a_values[m]);
			return out;
		};
		json summary = json::array();
		for (const auto& entry : a_result.summary)
			summary.push_back(json{ { "range", entry.name }, { "perFrame", entry.occurrencesPerFrame }, { "incomplete", entry.incomplete },
				{ "values", values(entry.values) } });
		json out{ { "captureId", a_result.id }, { "success", a_result.success }, { "error", a_result.error }, { "chip", a_result.chipName },
			{ "startFrame", a_result.startFrame }, { "endFrame", a_result.endFrame }, { "samples", a_result.samples },
			{ "passesPerSample", a_result.passesPerSample }, { "droppedRanges", a_result.droppedRanges },
			{ "droppedTraceBytes", a_result.droppedTraceBytes }, { "ranges", a_result.ranges }, { "metrics", std::move(metrics) },
			{ "unsupportedMetrics", std::move(unsupported) }, { "summary", std::move(summary) }, { "csvPath", Util::PathHelpers::ToUtf8(a_result.csvPath) } };
		if (a_detail) {
			json ranges = json::array();
			for (const auto& range : a_result.rangeResults)
				ranges.push_back(json{ { "sample", range.sample }, { "range", range.name }, { "path", range.path }, { "occurrence", range.occurrence },
					{ "values", values(range.values) } });
			out["rangeResults"] = std::move(ranges);
		}
		return out;
	}

	/**
	 * @brief Builds the response of the openshaders.nvperf tool.
	 *
	 * Captures run over the next frames on the render thread, so `capture` only queues one and returns its id; poll
	 * `result` (or wait for the openshaders.nvperf.completed event) for its counters. No call here touches the
	 * immediate context, so none marshals to the main thread.
	 */
	json BuildNvPerfResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" } };

		if (action == "status") {
			NvPerf::Probe();
			const auto device = NvPerf::GetDeviceInfo();
			return json{ { "built", device.built }, { "available", device.available }, { "gpuSupported", device.gpuSupported },
				{ "device", device.deviceName }, { "chip", device.chipName }, { "deviceError", device.error },
				{ "library", Util::PathHelpers::ToUtf8(device.libraryPath) }, { "capture", StatusJson(NvPerf::GetStatus()) } };
		}

		if (action == "metrics") {
			json metrics = json::array();
			for (const auto& metric : NvPerf::DefaultMetrics())
				metrics.push_back(MetricJson(metric));
			return json{ { "defaults", std::move(metrics) },
				{ "kinds", json::array({ "counter", "ratio", "throughput", "active", "elapsed", "percycle" }) } };
		}

		if (action == "capture") {
			NvPerf::CaptureConfiguration configuration;
			if (a_args.contains("metrics")) {
				if (!a_args["metrics"].is_array())
					return json{ { "error", "metrics must be an array of \"name[:kind]\" strings" } };
				for (const auto& entry : a_args["metrics"]) {
					if (!entry.is_string())
						return json{ { "error", "metrics must be an array of \"name[:kind]\" strings" } };
					NvPerf::MetricRequest metric;
					std::string error;
					if (!NvPerf::ParseMetric(entry.get<std::string>(), metric, error))
						return json{ { "error", error } };
					configuration.metrics.push_back(std::move(metric));
				}
			}
			if (a_args.contains("ranges")) {
				if (!a_args["ranges"].is_array())
					return json{ { "error", "ranges must be an array of strings" } };
				for (const auto& entry : a_args["ranges"]) {
					if (!entry.is_string() || entry.get<std::string>().empty())
						return json{ { "error", "ranges must be an array of non-empty strings" } };
					configuration.ranges.push_back(entry.get<std::string>());
				}
			}
			configuration.samples = a_args.value("samples", 1u);
			configuration.delayFrames = a_args.value("delayFrames", 0u);
			configuration.nestingLevels = static_cast<std::uint16_t>(std::clamp(a_args.value("nestingLevels", 1u), 0u, 0xFFFFu));
			std::uint64_t captureId = 0;
			std::string error;
			if (!NvPerf::RequestCapture(configuration, captureId, error))
				return json{ { "error", error } };
			return json{ { "queued", true }, { "captureId", captureId }, { "enqueued_at_frame", EnqueuedFrame() } };
		}

		if (action == "cancel") {
			NvPerf::CancelCapture();
			return json{ { "cancelRequested", true }, { "capture", StatusJson(NvPerf::GetStatus()) } };
		}

		if (action == "result") {
			const auto status = NvPerf::GetStatus();
			const auto result = NvPerf::LatestResult();
			const std::uint64_t wanted = a_args.value("captureId", result ? result->id : status.captureId);
			if (result && result->id == wanted)
				return CaptureResultJson(*result, a_args.value("detail", false));
			if (status.captureId == wanted && wanted)
				return json{ { "pending", true }, { "capture", StatusJson(status) } };
			return json{ { "error", "no such capture" }, { "captureId", wanted } };
		}

		return json{ { "error", "unknown action (status|metrics|capture|cancel|result)" }, { "action", action } };
	}

	void NvPerfToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildNvPerfResult, a_argsJson, a_sink, a_write);
	}

	/** @brief Starts a frame-locked burst on the capture provider; completion is the capture.burst.ready event. */
	json BuildBurstResult(const json& a_args)
	{
		OS::Capture::Burst::Request request;
		request.requestId = a_args.value("requestId", std::string{});
		request.prefix = a_args.value("prefix", std::string{});
		request.outputDir = Util::PathHelpers::FromUtf8(a_args.value("outputDir", std::string{}));
		request.manifestPath = Util::PathHelpers::FromUtf8(a_args.value("manifestPath", std::string{}));
		request.count = a_args.value("count", 0u);
		request.everyNFrames = a_args.value("everyNFrames", 1u);
		request.excludeUi = a_args.value("excludeUi", true);
		if (a_args.value("scale", 1.0f) != 1.0f)
			return json{ { "error", "burst captures full native frames; scale is not supported" } };
		if (a_args.contains("subrect") && !a_args["subrect"].is_null())
			return json{ { "error", "burst captures full native frames; subrect is not supported" } };
		const auto emit = [](const char* a_topic, const OS::Capture::Burst::Status& a_status) {
			json payload{ { "requestId", a_status.requestId }, { "captured", a_status.captured }, { "dropped", a_status.dropped } };
			if (std::string_view(a_topic) == "capture.burst.ready") {
				payload["ok"] = a_status.failed == 0 && a_status.captured > 0 && !a_status.manifestPath.empty();
				payload["width"] = a_status.width;
				payload["height"] = a_status.height;
				payload["uiExcluded"] = a_status.uiExcluded;
				payload["manifestPath"] = Util::PathToUtf8(a_status.manifestPath);
				if (!a_status.error.empty())
					payload["error"] = a_status.error;
				else if (a_status.failed)
					payload["error"] = std::format("{} frame(s) could not be captured or written", a_status.failed);
			}
			const std::string text = DumpForDevBench(payload);
			if (auto* dvb = DevBenchAPI::GetDevBenchInterface001())
				dvb->EmitEvent(a_topic, text.c_str());
		};
		const uint frame = EnqueuedFrame();
		return RunOnMainThread([request = std::move(request), emit, frame]() mutable -> json {
			auto* shot = &globals::features::screenshotFeature;
			if (!shot->loaded)
				return json{ { "error", "Screenshot feature is not loaded" } };
			uint64_t runId = 0;
			std::string error;
			if (!shot->StartBurst(
					std::move(request),
					[emit](const OS::Capture::Burst::Status& a_status) { emit("capture.burst.progress", a_status); },
					[emit](const OS::Capture::Burst::Status& a_status) { emit("capture.burst.ready", a_status); },
					runId, error))
				return json{ { "error", error } };
			return json{ { "queued", true }, { "provider", "openshaders" }, { "runId", runId }, { "enqueued_at_frame", frame } };
		});
	}

	json BuildBurstStatusResult(const json& a_args)
	{
		const auto runId = a_args.value("runId", uint64_t{ 0 });
		return RunOnMainThread([runId]() -> json {
			const auto status = globals::features::screenshotFeature.GetBurstStatus(runId);
			if (!status)
				return json{ { "error", "no such burst" } };
			return json{ { "runId", status->runId }, { "active", status->active }, { "done", status->finished }, { "requested", status->requested },
				{ "saved", status->captured }, { "failed", status->failed }, { "dropped", status->dropped }, { "manifest", Util::PathToUtf8(status->manifestPath) } };
		});
	}

	// The file is written on the next present; completion is the capture.ready event, not this return.
	json BuildCaptureProviderResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action == "burst")
			return BuildBurstResult(a_args);
		if (action == "burstStatus")
			return BuildBurstStatusResult(a_args);

		const std::string outputPath = a_args.value("outputPath", std::string{});
		const std::string requestId = a_args.value("requestId", std::string{});
		if (outputPath.empty() || requestId.empty())
			return json{ { "error", "capture provider needs outputPath and requestId" } };

		ScreenshotFeature::CaptureRequest request;
		request.outputPath = Util::PathHelpers::FromUtf8(outputPath);
		if (!request.outputPath.is_absolute() || request.outputPath.extension() != ".png")
			return json{ { "error", "outputPath must be an absolute .png path" } };
		request.excludeUi = a_args.value("excludeUi", true);
		if (a_args.contains("subrect") && a_args["subrect"].is_object()) {
			const auto& rect = a_args["subrect"];
			const Util::Subrect::UVRegion region{ rect.value("x", 0.0f), rect.value("y", 0.0f), rect.value("w", 1.0f), rect.value("h", 1.0f) };
			if (!std::isfinite(region.x) || !std::isfinite(region.y) || !std::isfinite(region.w) || !std::isfinite(region.h) || region.w <= 0.0f || region.h <= 0.0f)
				return json{ { "error", "subrect must be finite with a positive w and h" } };
			if (region.x < 0.0f || region.y < 0.0f || region.x + region.w > 1.0f || region.y + region.h > 1.0f)
				return json{ { "error", "subrect must lie within 0..1 UV" } };
			request.region = region;
		}
		request.onComplete = [requestId](const ScreenshotFeature::CaptureResult& a_result) {
			json payload{ { "requestId", requestId }, { "ok", a_result.ok }, { "uiExcluded", a_result.uiExcluded },
				{ "width", a_result.width }, { "height", a_result.height } };
			if (a_result.ok)
				payload["renderFrame"] = a_result.renderFrame;
			if (!a_result.ok)
				payload["error"] = "the screenshot could not be captured or written";
			const std::string text = payload.dump();
			if (auto* dvb = DevBenchAPI::GetDevBenchInterface001())
				dvb->EmitEvent("capture.ready", text.c_str());
		};
		const uint frame = EnqueuedFrame();
		return RunOnMainThread([request = std::move(request), frame]() mutable -> json {
			auto* shot = &globals::features::screenshotFeature;
			if (!shot->loaded)
				return json{ { "error", "Screenshot feature is not loaded" } };
			if (!shot->RequestCapture(std::move(request)))
				return json{ { "error", "another capture is already waiting or a frame sequence is active" } };
			return json{ { "queued", true }, { "provider", "openshaders" }, { "enqueued_at_frame", frame } };
		});
	}

	void CaptureProviderHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildCaptureProviderResult, a_argsJson, a_sink, a_write);
	}

	void ScreenshotReferenceHandler(void* context, const char* args, void* sink, DevBenchAPI::WriteFn write) noexcept
	{
		json result;
		try {
			const auto request = json::parse(args ? args : "{}");
			auto* host = static_cast<DevBenchAPI::IDevBenchInterface001*>(context);
			result = RunOnMainThread([request, host] {
				return globals::features::screenshotFeature.HandleReferenceCapture(request, [host](const json& completion) {
					const auto text = DumpForDevBench(completion);
					host->EmitEvent("capture.ready", text.c_str());
				});
			});
			if (!result.value("ok", false))
				result = { { "error", result.at("error").is_string() ? result.at("error").get<std::string>() : result.at("error").value("message", "Reference capture failed") } };
		} catch (const std::exception& error) {
			result = { { "error", error.what() } };
		} catch (...) {
			result = { { "error", "Reference capture failed" } };
		}
		try {
			const auto text = DumpForDevBench(result);
			write(sink, text.c_str());
		} catch (...) {
			write(sink, R"({"error":"Reference result serialization failed"})");
		}
	}

	// ---- settings: save / load / reset the GLOBAL CS config ---------------------------

	json BuildSettingsResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" } };
		auto* state = globals::state;
		if (!state)
			return json{ { "error", "State singleton unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// State::Save/Load read and write the on-disk USER config and touch every feature's
		// settings; both must run on the main thread for the same reason feature(set) does.
		// Contain failures inside the task: RunHandler's guard no longer applies once this runs
		// on SKSE's queue, so a malformed config / Save|Load throw would unwind on the game
		// thread after we already replied "queued". Degrade to a logged error instead.
		if (action == "save") {
			task->AddTask([state]() {
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(save) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(save) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(save) failed (unknown)");
				}
			});
			return json{ { "action", "save" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "load") {
			task->AddTask([state]() {
				try {
					state->Load(State::ConfigMode::USER, /*allowReload=*/true);
					logger::info("DevBenchBridge: settings(load) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(load) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(load) failed (unknown)");
				}
			});
			return json{ { "action", "load" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "reset") {
			// Restore every feature to its defaults, then persist. Mirrors what the UI's
			// global reset does: per-feature RestoreDefaultSettings followed by a Save.
			task->AddTask([state]() {
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard(
					*SceneSettingsManager::GetSingleton());
				for (auto* f : Feature::GetFeatureList()) {
					try {
						f->RestoreDefaultSettings();
					} catch (const std::exception& e) {
						logger::error("DevBenchBridge: settings(reset) {} threw: {}", f->GetShortName(), e.what());
					}
				}
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(reset) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(reset) save failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(reset) save failed (unknown)");
				}
			});
			return json{ { "action", "reset" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "applyVRProfile") {
			// Same broadcast path as the in-game hub button, exposed for headless automation/CI.
			const std::string profileName = a_args.value("profile", std::string{});
			Feature::PerfProfile profile;
			if (profileName == "performance")
				profile = Feature::PerfProfile::Performance;
			else if (profileName == "balanced")
				profile = Feature::PerfProfile::Balanced;
			else if (profileName == "quality")
				profile = Feature::PerfProfile::Quality;
			else
				return json{ { "error", "unknown profile (performance|balanced|quality)" }, { "profile", profileName } };

			task->AddTask([state, profile]() {
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard(
					*SceneSettingsManager::GetSingleton());
				try {
					Feature::ApplyPerformanceProfileToAll(profile);
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(applyVRProfile) threw: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(applyVRProfile) threw (unknown)");
				}
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(applyVRProfile) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(applyVRProfile) save failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(applyVRProfile) save failed (unknown)");
				}
			});
			return json{ { "action", "applyVRProfile" }, { "profile", profileName }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		return json{ { "error", "unknown action (save|load|reset|applyVRProfile)" }, { "action", action } };
	}

	void SettingsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildSettingsResult, a_argsJson, a_sink, a_write);
	}

	// ---- menu handler: open / close / toggle the Community Shaders settings menu -------

	/** @brief Longest tab label the menu call accepts; real tab names are far shorter. */
	constexpr size_t kMaxTabLabelLength = 128;

	json BuildMenuResult(const json& a_args)
	{
		const auto editorMode = a_args.find("editorMode");
		if (editorMode != a_args.end() && (!editorMode->is_string() || (*editorMode != "browser" && *editorMode != "menu")))
			return json{ { "error", "editorMode must be browser or menu" } };
		const auto tabArg = a_args.find("tab");
		if (tabArg != a_args.end() && (!tabArg->is_string() || tabArg->get_ref<const std::string&>().size() > kMaxTabLabelLength))
			return json{ { "error", std::format("tab must be a string of at most {} characters", kMaxTabLabelLength) } };
		const auto sidebarVisibility = a_args.find("sidebarVisible");
		if (sidebarVisibility != a_args.end() && !sidebarVisibility->is_boolean())
			return json{ { "error", "sidebarVisible must be a boolean" } };

		const std::string op = a_args.value("op", std::string("toggle"));
		Menu::VisibilityRequest req;
		if (op == "open")
			req = Menu::VisibilityRequest::Open;
		else if (op == "close")
			req = Menu::VisibilityRequest::Close;
		else if (op == "toggle")
			req = Menu::VisibilityRequest::Toggle;
		else
			return json{ { "error", "unknown op (open|close|toggle)" }, { "op", op } };

		// Optional page navigation, e.g. "Performance" or a feature's GetShortName().
		// RequestFeatureMenu is thread-safe (see its doc comment) for the same reason
		// RequestVisibility is used below instead of calling SetVisible directly.
		std::string page;
		if (auto it = a_args.find("page"); it != a_args.end() && it->is_string())
			page = it->get<std::string>();
		if (!page.empty())
			Menu::GetSingleton()->RequestFeatureMenu(page);
		const std::string tab = tabArg != a_args.end() ? tabArg->get<std::string>() : std::string{};
		if (!tab.empty())
			Menu::GetSingleton()->RequestTab(tab);

		// SetVisible touches the ImGui context and the IsEnabled flag the render thread owns, so
		// it can't run on this listener thread (nor on the SKSE main thread). Enqueue an atomic
		// request the render loop consumes next frame, mirroring the ToggleKey path.
		Menu::GetSingleton()->RequestVisibility(req);
		if (sidebarVisibility != a_args.end())
			Menu::GetSingleton()->RequestSidebarVisibility(sidebarVisibility->get<bool>());
		if (editorMode != a_args.end())
			EditorWindow::GetSingleton()->RequestBrowserMode(*editorMode == "menu" ? EditorWindow::BrowserMode::Menu : EditorWindow::BrowserMode::Editor);
		return json{ { "op", op }, { "page", page }, { "tab", tab }, { "queued", true } };
	}

	void MenuHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildMenuResult, a_argsJson, a_sink, a_write);
	}

	// ---- pluginapi: exercise the real SKSE ICSInterface001 ABI ------------------------

	// GetApi(0) always returns the same static instance for any revision this build
	// supports (see CSpluginapi.cpp) -- no handshake needed since we're in-process.
	CSPluginAPI::ICSInterface001* GetTestPluginApi()
	{
		return static_cast<CSPluginAPI::ICSInterface001*>(CSPluginAPI::GetApi(0));
	}

	json WindVectorToJson(const CSPluginAPI::WindVector& a_vector)
	{
		return json::array({ a_vector.x, a_vector.y, a_vector.z });
	}

	json WindSampleToJson(const CSPluginAPI::WindSample& a_sample)
	{
		return json{
			{ "available", true },
			{ "baseVelocity", WindVectorToJson(a_sample.baseVelocity) },
			{ "gustVelocity", WindVectorToJson(a_sample.gustVelocity) },
			{ "transientVelocity", WindVectorToJson(a_sample.transientVelocity) },
			{ "finalVelocity", WindVectorToJson(a_sample.finalVelocity) },
			{ "ambientGust", a_sample.ambientGust },
			{ "transientIntensity", a_sample.transientIntensity },
			{ "frameId", a_sample.frameId }
		};
	}

	// Dispatches directly on the devbench listener thread rather than marshaling to
	// main: API.md documents every method as callable from any thread (setters stage
	// into CSPluginAPI's queue; ProcessStagedSettings applies on the render thread next
	// frame), so calling from here exercises that contract instead of sidestepping it.
	json BuildPluginApiResult(const json& a_args)
	{
		auto* api = GetTestPluginApi();
		if (!api)
			return json{ { "error", "plugin API unavailable" } };

		const std::string method = a_args.value("method", std::string{});
		const json params = a_args.value("params", json::object());
		const uint frame = EnqueuedFrame();

		using CSPluginAPI::DLSSProfile;
		using CSPluginAPI::UpscaleMethod;
		using CSPluginAPI::UpscalePreset;
		using Handler = std::function<json(CSPluginAPI::ICSInterface001*, const json&)>;

		static const std::unordered_map<std::string, Handler> kMethods{
			{ "getBuildNumber", [](auto* i, const json&) { return json{ { "result", i->getBuildNumber() } }; } },
			{ "GetSSSEnabled", [](auto* i, const json&) { return json{ { "result", i->GetSSSEnabled() } }; } },
			{ "SetSSSEnabled", [](auto* i, const json& p) { i->SetSSSEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetSSGIEnabled", [](auto* i, const json&) { return json{ { "result", i->GetSSGIEnabled() } }; } },
			{ "SetSSGIEnabled", [](auto* i, const json& p) { i->SetSSGIEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetVolumetricLightingExteriorEnabled", [](auto* i, const json&) { return json{ { "result", i->GetVolumetricLightingExteriorEnabled() } }; } },
			{ "SetVolumetricLightingExteriorEnabled", [](auto* i, const json& p) { i->SetVolumetricLightingExteriorEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetUpscalePreset", [](auto* i, const json&) { return json{ { "result", static_cast<uint32_t>(i->GetUpscalePreset()) } }; } },
			{ "SetUpscalePreset", [](auto* i, const json& p) { i->SetUpscalePreset(static_cast<UpscalePreset>(p.value("preset", 0u))); return json{ { "staged", true } }; } },
			{ "GetLightLimitFixContactShadowsEnabled", [](auto* i, const json&) { return json{ { "result", i->GetLightLimitFixContactShadowsEnabled() } }; } },
			{ "SetLightLimitFixContactShadowsEnabled", [](auto* i, const json& p) { i->SetLightLimitFixContactShadowsEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetDLSSProfile", [](auto* i, const json&) { return json{ { "result", static_cast<uint32_t>(i->GetDLSSProfile()) } }; } },
			{ "SetDLSSProfile", [](auto* i, const json& p) { i->SetDLSSProfile(static_cast<DLSSProfile>(p.value("profile", 0u))); return json{ { "staged", true } }; } },
			{ "GetRenderAtUpscaleResEnabled", [](auto* i, const json&) { return json{ { "result", i->GetRenderAtUpscaleResEnabled() } }; } },
			{ "SetRenderAtUpscaleResEnabled", [](auto* i, const json& p) { i->SetRenderAtUpscaleResEnabled(p.value("enabled", false)); return json{ { "staged", true } }; } },
			{ "GetRenderAtUpscaleResActive", [](auto* i, const json&) { return json{ { "result", i->GetRenderAtUpscaleResActive() } }; } },
			{ "SetVRUpscalingTransitionProfile", [](auto* i, const json& p) {
				 i->SetVRUpscalingTransitionProfile(p.value("renderScaleModeEnabled", false), static_cast<UpscalePreset>(p.value("preset", 0u)), static_cast<DLSSProfile>(p.value("profile", 0u)));
				 return json{ { "staged", true } }; } },
			{ "GetUpscaleMethod", [](auto* i, const json&) { return json{ { "result", static_cast<uint32_t>(i->GetUpscaleMethod()) } }; } },
			{ "SetUpscaleMethod", [](auto* i, const json& p) { i->SetUpscaleMethod(static_cast<UpscaleMethod>(p.value("method", 0u))); return json{ { "staged", true } }; } },
			{ "SetVRUpscalingTransitionProfileForMethod", [](auto* i, const json& p) {
				 i->SetVRUpscalingTransitionProfileForMethod(static_cast<UpscaleMethod>(p.value("method", 0u)), p.value("renderScaleModeEnabled", false), static_cast<UpscalePreset>(p.value("preset", 0u)), static_cast<DLSSProfile>(p.value("profile", 0u)));
				 return json{ { "staged", true } }; } },
			{ "GetVRUpscalingApplyBlockReasons", [](auto* i, const json&) { return json{ { "result", i->GetVRUpscalingApplyBlockReasons() } }; } },
			{ "IsVRUpscalingProfileApplyAllowed", [](auto* i, const json&) { return json{ { "result", i->IsVRUpscalingProfileApplyAllowed() } }; } },
			{ "SampleWind", [](auto* i, const json& p) {
				 const CSPluginAPI::WindVector position{
					 p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f)
				 };
				 CSPluginAPI::WindSample sample;
				 if (!i->SampleWind(&position, &sample, 1))
					 return json{ { "available", false } };
				 return WindSampleToJson(sample);
			 } },
			{ "SampleWindExcludingHavokImpulses", [](auto* i, const json& p) {
				 const CSPluginAPI::WindVector position{
					 p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f)
				 };
				 CSPluginAPI::WindSampleWithHavokExclusion sample;
				 if (!i->SampleWindExcludingHavokImpulses(&position, &sample, 1))
					 return json{ { "available", false } };
				 json result = WindSampleToJson(sample.wind);
				 result["windExcludingHavokImpulses"] = WindVectorToJson(sample.windExcludingHavokImpulses);
				 return result;
			 } },
		};

		const auto it = kMethods.find(method);
		if (it == kMethods.end())
			return json{ { "error", "unknown method" }, { "method", method } };
		try {
			json result = it->second(api, params);
			// The ABI setters return void, so "staged" means dispatched, not
			// accepted -- the ABI's own enum validation can silently no-op an
			// unsupported value. Stamp the frame so a caller can poll
			// inspect/openshaders.feature for the actual outcome next tick.
			if (result.contains("staged"))
				result["enqueued_at_frame"] = frame;
			return result;
		} catch (const std::exception& e) {
			return json{ { "error", "method threw" }, { "method", method }, { "detail", e.what() } };
		}
	}

	void PluginApiHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildPluginApiResult, a_argsJson, a_sink, a_write);
	}
}

namespace DevBenchBridge
{
	void Install()
	{
		auto* dvb = DevBenchAPI::GetDevBenchInterface001();
		if (!dvb) {
			logger::info("DevBenchBridge: devbench not present; CS tools not registered");
			return;
		}
		logger::info("DevBenchBridge: devbench build {} present — registering CS tools", dvb->GetBuildNumber());

		// Namespaced tool names — devbench's registry is shared across plugins, so bare
		// names ("feature", "inspect"…) could collide with devbench's own or another mod's.
		// Descriptors preserve the actions/kinds/inputSchema of CS's former embedded server
		// so existing MCP clients keep working under the new prefix.

		dvb->RegisterTool("openshaders.feature", BrandedDescription(ToolSpecs::kFeatureTool), &FeatureToolHandler, nullptr);

		// One-time: builds Util::DevBenchUx::Registry from every feature's
		// RegisterUxActions() override, so openshaders.actions below has something to dispatch.
		RegisterDevBenchUx();

		dvb->RegisterTool("openshaders.actions", BrandedDescription(ToolSpecs::kActionsTool), &ActionsToolHandler, nullptr);

		dvb->RegisterTool("openshaders.shadercache", BrandedDescription(ToolSpecs::kShadercacheTool), &ShadercacheToolHandler, nullptr);

		dvb->RegisterTool("openshaders.lighthighlight", BrandedDescription(ToolSpecs::kLightHighlightTool), &LightHighlightToolHandler, nullptr);

		dvb->RegisterTool("openshaders.profiler", BrandedDescription(ToolSpecs::kProfilerTool), &ProfilerToolHandler, nullptr);

		dvb->RegisterTool("openshaders.capture", BrandedDescription(ToolSpecs::kCaptureTool), &CaptureToolHandler, nullptr);

		dvb->RegisterTool("openshaders.nvperf", BrandedDescription(ToolSpecs::kNvPerfTool), &NvPerfToolHandler, nullptr);
		NvPerf::SetCompletionCallback([](const NvPerf::CaptureResult& a_result) {
			if (auto* host = DevBenchAPI::GetDevBenchInterface001()) {
				const std::string payload = DumpForDevBench(json{ { "captureId", a_result.id }, { "success", a_result.success }, { "error", a_result.error } });
				host->EmitEvent("openshaders.nvperf.completed", payload.c_str());
			}
		});

		dvb->RegisterTool("openshaders.settings", BrandedDescription(ToolSpecs::kSettingsTool), &SettingsToolHandler, nullptr);

		dvb->RegisterTool("openshaders.pluginapi", BrandedDescription(ToolSpecs::kPluginApiTool), &PluginApiHandler, nullptr);

		// devbench 1.5.0+ generalized tool extensions: route the CS settings menu and the
		// non-feature reads UNDER the base `menu` / `inspect` tools (menu invoke name=…,
		// inspect kind=…) instead of as top-level tools, keeping the agent-facing surface
		// small. RegisterToolExtension's vtable slot exists only on build >= 10500; no
		// fallback on older hosts (the menu + inspect reads are simply unavailable there).
		// "CommunityShaders" matches the ImGui window id after `###`, so the menu name lines
		// up with the on-screen window.
		if (dvb->GetBuildNumber() >= 10500) {
			dvb->RegisterToolExtension("capture", "openshaders.native", ToolSpecs::kCaptureNativeExt, &ScreenshotReferenceHandler, dvb);
			dvb->RegisterToolExtension("menu", "CommunityShaders", BrandedDescription(ToolSpecs::kMenuExt), &MenuHandler, nullptr);

			dvb->RegisterToolExtension("inspect", "openshaders", BrandedDescription(ToolSpecs::kInspectStateExt), &InspectStateHandler, nullptr);

			dvb->RegisterToolExtension("inspect", "shadercache", BrandedDescription(ToolSpecs::kInspectShadercacheExt), &InspectShadercacheHandler, nullptr);

			dvb->RegisterToolExtension("inspect", "llfshadows", BrandedDescription(ToolSpecs::kInspectLlfShadowsExt), &InspectShadowsHandler, nullptr);

			dvb->RegisterToolExtension("inspect", "profiler", BrandedDescription(ToolSpecs::kInspectProfilerExt), &InspectProfilerHandler, nullptr);

			dvb->RegisterToolExtension("inspect", "featureissues", BrandedDescription(ToolSpecs::kInspectFeatureIssuesExt), &InspectFeatureIssuesHandler, nullptr);

			dvb->RegisterToolExtension("capture", "openshaders", BrandedDescription(ToolSpecs::kCaptureProviderExt), &CaptureProviderHandler, nullptr);
		} else {
			logger::info("DevBenchBridge: devbench build {} < 10500; CS menu + inspect extensions need 1.5.0", dvb->GetBuildNumber());
		}
	}
}

#else

namespace DevBenchBridge
{
	void Install() {}  // inert until built with DEVBENCH_BRIDGE_ENABLED
}

#endif
