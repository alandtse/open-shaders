#include "DevBenchBridge.h"

// Registers our tools into the devbench test bench over its C-ABI. Gated by
// DEVBENCH_BRIDGE_ENABLED (set by CMake when the devbench-api port is available);
// otherwise this file compiles to an empty Install(). Inert at runtime when no
// devbench plugin is present (GetDevBenchInterface001() returns null).
//
// The openshaders.* tools below expose Open Shaders' graphics-feature, inspect,
// capture, abtest, shadercache, and settings operations through the single devbench
// host over both MCP and REST. Each is namespaced to avoid collisions in devbench's
// shared registry; the action / kind / inputSchema shapes are stable so clients can
// rely on them.

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Feature.h"
#	include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#	include "Features/RenderDoc.h"
#	include "Features/ScreenshotFeature.h"
#	include "Globals.h"
#	include "ShaderCache.h"
#	include "State.h"

#	include <DevBenchAPI.h>
#	include <nlohmann/json.hpp>

#	include <algorithm>
#	include <chrono>
#	include <cstring>
#	include <functional>
#	include <future>
#	include <memory>
#	include <optional>
#	include <stdexcept>

namespace
{
	using json = nlohmann::json;

	// Current render frame, used as a coarse "enqueued at" stamp so callers can poll
	// inspect(kind=state) until frame_count advances past it (i.e. a queued main-thread
	// task has had at least one tick to run). Safe from any thread (atomic load).
	uint EnqueuedFrame()
	{
		return globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
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
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	// Run a READ on the main thread and return its result. Bridge handlers run on devbench's
	// listener thread; reads that touch state the render/UI thread mutates (A/B fields + JSON
	// snapshots) must marshal or they race. Blocks the handler briefly, bounded so a stalled
	// main thread (e.g. mid-load) can't hang it. shared_ptr promise so a timed-out task that
	// runs later doesn't dangle.
	json RunReadOnMainThread(std::function<json()> a_read)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		auto prom = std::make_shared<std::promise<json>>();
		auto fut = prom->get_future();
		task->AddTask([prom, read = std::move(a_read)]() {
			try {
				prom->set_value(read());
			} catch (const std::exception& e) {
				prom->set_value(json{ { "error", "read threw on main thread" }, { "detail", e.what() } });
			}
		});
		if (fut.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready)
			return json{ { "error", "main thread did not run within 5000ms (mid-load?)" } };
		return fut.get();
	}

	// ---- feature: list / get / set / reset / toggle -----------------------------------

	// Build one feature entry, including restart-gated metadata so `list` answers
	// "what exists", "which fields need a restart", and "is anything pending" in one read.
	json FeatureEntry(Feature* f)
	{
		json entry{
			{ "name", f->GetName() },
			{ "shortName", f->GetShortName() },
			{ "loaded", f->loaded },
			{ "version", f->version },
			{ "category", std::string(f->GetCategory()) },
			{ "isCore", f->IsCore() },
			{ "supportsVR", f->SupportsVR() },
			{ "inMenu", f->IsInMenu() },
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
			return RunReadOnMainThread([]() {
				json out = json::array();
				for (auto* f : Feature::GetFeatureList())
					out.push_back(FeatureEntry(f));
				return out;
			});
		}

		const std::string shortName = a_args.value("shortName", std::string{});

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
				// Don't let a remote caller enable a VR-incompatible feature on a VR runtime:
				// it bypasses the SupportsVR() gate and can destabilize the renderer. Reject +
				// report (covers both an explicit enable and an implicit flip resolving to true).
				if (applied && REL::Module::IsVR() && !target->SupportsVR()) {
					if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
						const std::string payload = json{ { "shortName", shortName }, { "error", "feature does not support VR; enable rejected" } }.dump();
						dvb->EmitEvent("openshaders.feature.changed", payload.c_str());
					}
					logger::warn("DevBenchBridge: refused to enable VR-unsupported feature '{}' on a VR runtime", shortName);
					return;
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
			return RunReadOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				json blob;
				feature->SaveSettings(blob);
				return blob;
			});
		}

		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" }, { "shortName", shortName } };
		const uint frame = EnqueuedFrame();

		if (action == "set") {
			if (!a_args.contains("settings") || !a_args["settings"].is_object())
				return json{ { "error", "missing required object parameter 'settings'" } };
			json blob = a_args["settings"];
			// Resolve + apply on the main thread (LoadSettings mutates UI/render state and the
			// lookup reads loaded). Not-found degrades to a logged warning (fire-and-forget).
			task->AddTask([blob, shortName]() mutable {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature) {
					logger::warn("DevBenchBridge: feature(set, {}) not found or not loaded", shortName);
					return;
				}
				try {
					feature->LoadSettings(blob);
					logger::info("DevBenchBridge: feature(set, {}) applied", shortName);
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: feature(set, {}) LoadSettings threw: {}", shortName, e.what());
				}
			});
			return json{ { "action", "set" }, { "shortName", shortName }, { "queued", true }, { "enqueued_at_frame", frame } };
		}

		if (action == "reset") {
			task->AddTask([shortName]() {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature) {
					logger::warn("DevBenchBridge: feature(reset, {}) not found or not loaded", shortName);
					return;
				}
				try {
					feature->RestoreDefaultSettings();
					logger::info("DevBenchBridge: feature(reset, {}) applied", shortName);
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: feature(reset, {}) RestoreDefaultSettings threw: {}", shortName, e.what());
				}
			});
			return json{ { "action", "reset" }, { "shortName", shortName }, { "queued", true }, { "enqueued_at_frame", frame } };
		}

		return json{ { "error", "unknown action (list|get|set|reset|toggle)" }, { "action", action } };
	}

	void FeatureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildFeatureResult, a_argsJson, a_sink, a_write);
	}

	// ---- inspect: engine state / shader-cache status ----------------------------------

	json BuildInspectResult(const json& a_args)
	{
		const std::string kind = a_args.value("kind", std::string{});
		if (kind.empty())
			return json{ { "error", "missing required parameter 'kind'" } };

		if (kind == "state") {
			return json{
				{ "plugin", "CommunityShaders" },
				{ "frame_count", EnqueuedFrame() },
				{ "vr", REL::Module::IsVR() },
			};
		}
		if (kind == "shadercache") {
			// Built from thread-safe ShaderCache accessors. Poll completedTasks against a
			// pre-deploy snapshot to know a hot-reloaded shader finished; a rising
			// failedTasks / currentFailedCount surfaces an otherwise-invisible failed compile.
			auto* cache = globals::shaderCache;
			if (!cache)
				return json{ { "error", "shader cache unavailable" } };
			return json{
				{ "compiling", cache->IsCompiling() },
				{ "completedTasks", cache->GetCompletedTasks() },
				{ "totalTasks", cache->GetTotalTasks() },
				{ "failedTasks", cache->GetFailedTasks() },
				{ "currentFailedCount", cache->GetCurrentFailedCount() },
				{ "frame_count", EnqueuedFrame() },
			};
		}
		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "state", "shadercache" }) } };
	}

	void InspectToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectResult, a_argsJson, a_sink, a_write);
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
		// — marshal to the main thread. Both force a recompile, observable via
		// inspect(kind=shadercache) + the openshaders.shaderRecompiled event.
		if (action == "clear") {
			task->AddTask([cache]() { cache->Clear(); });
			return json{ { "action", "clear" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "in-memory cache cleared; shaders requeue for recompile" } };
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
		return json{ { "error", "unknown action (clear|deleteDisk)" }, { "action", action } };
	}

	void ShadercacheToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildShadercacheResult, a_argsJson, a_sink, a_write);
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
			return RunReadOnMainThread([frameCount, frame]() -> json {
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
			return RunReadOnMainThread([frame]() -> json {
				auto* shot = &globals::features::screenshotFeature;
				if (!shot->loaded)
					return json{ { "error", "Screenshot feature is not loaded" } };
				shot->captureRequested.store(true, std::memory_order_release);
				return json{ { "queued", true }, { "kind", "screenshot" }, { "enqueued_at_frame", frame } };
			});
		}

		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "renderdoc", "screenshot" }) } };
	}

	void CaptureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildCaptureResult, a_argsJson, a_sink, a_write);
	}

	// ---- abtest: status / start / stop / clear / diff ---------------------------------

	json AbtestStatusBlob(ABTestingManager* a_mgr)
	{
		return json{
			{ "enabled", a_mgr->IsEnabled() },
			{ "usingTestConfig", a_mgr->IsUsingTestConfig() },
			{ "interval", a_mgr->GetTestInterval() },
			{ "hasCachedSnapshots", a_mgr->HasCachedSnapshots() },
		};
	}

	json BuildAbtestResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" } };
		auto* mgr = ABTestingManager::GetSingleton();
		if (!mgr)
			return json{ { "error", "ABTestingManager singleton unavailable" } };

		// status/diff read ABTestingManager's plain fields + JSON snapshots, which the main
		// thread mutates in Enable/Disable/Update/Clear — marshal the reads so they don't race.
		if (action == "status")
			return RunReadOnMainThread([mgr]() { return AbtestStatusBlob(mgr); });

		if (action == "diff") {
			return RunReadOnMainThread([mgr]() {
				json entries = json::array();
				for (const auto& entry : mgr->GetConfigDiffEntries()) {
					// SettingsDiffEntry uses generic a/b labels; here `a` is USER, `b` is TEST.
					entries.push_back(json{
						{ "path", entry.path },
						{ "userValue", entry.aValue },
						{ "testValue", entry.bValue },
					});
				}
				return json{ { "hasCachedSnapshots", mgr->HasCachedSnapshots() }, { "entries", std::move(entries) } };
			});
		}

		// results: the aggregated per-variant timing — the benchmark output. Marshaled (the
		// aggregator is written on the main thread during Update/OnFrame).
		if (action == "results") {
			return RunReadOnMainThread([mgr]() {
				auto& agg = mgr->GetAggregator();
				json calls = json::array();
				for (const auto& s : agg.GetAggregatedResults()) {
					calls.push_back(json{
						{ "label", s.label },
						{ "shaderType", s.shaderType },
						{ "meanA", s.meanA },
						{ "meanB", s.meanB },
						{ "delta", s.delta },
						{ "medianA", s.medianA },
						{ "medianB", s.medianB },
						{ "frameCountA", s.frameCountA },
						{ "frameCountB", s.frameCountB },
					});
				}
				return json{
					{ "hasResults", agg.HasResults() },
					{ "totalFrames", agg.GetTotalFrameCount() },
					{ "totalDurationSec", agg.GetTotalTestDuration() },
					{ "drawCalls", std::move(calls) },
				};
			});
		}

		// Lifecycle actions marshal onto the main thread: Enable/Disable swap configs via
		// State::Load → JSON and Menu::Load touches settings the menu/render thread reads.
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();
		// Enqueue metadata only — NOT a status blob: reading ABTestingManager's fields here (on
		// the listener thread) would race the main thread that mutates them. Callers query
		// status separately (status/diff marshal the read).
		const auto queued = [&](const char* act) {
			return json{ { "action", act }, { "queued", true }, { "enqueued_at_frame", frame } };
		};

		if (action == "start") {
			std::optional<uint32_t> interval;
			if (a_args.contains("interval") && a_args["interval"].is_number()) {
				const auto secs = a_args["interval"].get<int>();
				if (secs > 0)
					interval = static_cast<uint32_t>(secs);
			}
			const bool manual = a_args.value("manual", false);
			// Contain throws: Enable/Disable invoke State::Load (can throw on a malformed config)
			// and would otherwise unwind on the game thread after we already replied "queued".
			task->AddTask([mgr, interval, manual]() {
				try {
					if (interval)
						mgr->SetTestInterval(*interval);
					mgr->SetManualMode(manual);  // manual → caller drives swaps via action=switch
					mgr->Enable();
					logger::info("DevBenchBridge: abtest(start) applied (manual={})", manual);
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: abtest(start) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: abtest(start) failed (unknown)");
				}
			});
			return queued("start");
		}
		if (action == "stop") {
			task->AddTask([mgr]() {
				try {
					mgr->Disable();
					logger::info("DevBenchBridge: abtest(stop) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: abtest(stop) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: abtest(stop) failed (unknown)");
				}
			});
			return queued("stop");
		}
		if (action == "clear") {
			task->AddTask([mgr]() {
				try {
					mgr->ClearCachedSnapshots();
					logger::info("DevBenchBridge: abtest(clear) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: abtest(clear) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: abtest(clear) failed (unknown)");
				}
			});
			return queued("clear");
		}
		if (action == "switch") {
			// Manual swap (USER<->TEST) for a started manual-mode session — lets a driver align
			// switches to whole benchmark passes instead of the timer. No-op if not enabled.
			task->AddTask([mgr]() {
				try {
					mgr->SwitchVariant();
					logger::info("DevBenchBridge: abtest(switch) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: abtest(switch) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: abtest(switch) failed (unknown)");
				}
			});
			return queued("switch");
		}
		return json{ { "error", "unknown action (status|start|stop|clear|diff|switch|results)" }, { "action", action } };
	}

	void AbtestToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildAbtestResult, a_argsJson, a_sink, a_write);
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
		return json{ { "error", "unknown action (save|load|reset)" }, { "action", action } };
	}

	void SettingsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildSettingsResult, a_argsJson, a_sink, a_write);
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

		static constexpr const char* featureDesc =
			R"({"description":"All Open Shaders graphics-feature operations — enumerate, inspect settings, mutate settings, restore defaults, toggle on/off. Action-dispatched. list: returns an array of {name,shortName,loaded,version,category,isCore,supportsVR,inMenu}; features with restart-gated settings also include restartFields:[{key,label,pending}]. get: params shortName, returns the SaveSettings blob (null if the feature has no override; set/reset then no-op). set: params shortName, settings (object). reset: params shortName, calls RestoreDefaultSettings. toggle: params shortName, enabled (boolean, OPTIONAL — omit to flip the current loaded state); flips Feature::loaded.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["list","get","set","reset","toggle"]},"shortName":{"type":"string"},"settings":{"type":"object"},"enabled":{"type":"boolean"}}}})";
		dvb->RegisterTool("openshaders.feature", featureDesc, &FeatureToolHandler, nullptr);

		static constexpr const char* inspectDesc =
			R"({"description":"Read non-feature Open Shaders engine state. Kind-dispatched; response is a JSON object. kind=state -> {plugin,frame_count,vr}; frame_count increases each render tick, use it as ground truth that a queued operation has had time to run. kind=shadercache -> {compiling,completedTasks,totalTasks,failedTasks,currentFailedCount,frame_count}; poll completedTasks against a pre-deploy snapshot to know a hot-reloaded shader finished, and watch failedTasks/currentFailedCount for failed compiles. For feature reads use openshaders.feature(action=list|get).","readOnly":true,"inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["state","shadercache"]}},"required":["kind"]}})";
		dvb->RegisterTool("openshaders.inspect", inspectDesc, &InspectToolHandler, nullptr);

		static constexpr const char* shadercacheDesc =
			R"({"description":"Manage Open Shaders' compiled shader cache. Action-dispatched, fire-and-forget on the main thread. clear: drop the in-memory cache so shaders requeue for recompile. deleteDisk: delete the on-disk cache, forcing a full recompile (cold-compile benchmark). Watch progress via openshaders.inspect kind=shadercache and the openshaders.shaderRecompiled event. Read-only status is openshaders.inspect kind=shadercache.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["clear","deleteDisk"]}},"required":["action"]}})";
		dvb->RegisterTool("openshaders.shadercache", shadercacheDesc, &ShadercacheToolHandler, nullptr);

		static constexpr const char* captureDesc =
			R"({"description":"Trigger a frame capture on the next render. Kind-dispatched. kind=renderdoc: RenderDoc multi-frame capture via the in-app API, honors frames (1-120, default 1); RenderDoc must be attached/loaded (check openshaders.feature list for RenderDoc.loaded). kind=screenshot: lossless screenshot via the Screenshot feature; frames is ignored. Fire-and-forget — no artifact path is returned synchronously.","inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["renderdoc","screenshot"]},"frames":{"type":"number"}},"required":["kind"]}})";
		dvb->RegisterTool("openshaders.capture", captureDesc, &CaptureToolHandler, nullptr);

		static constexpr const char* abtestDesc =
			R"({"description":"Drive the built-in A/B testing harness (Performance Overlay/ABTesting), which swaps between the USER config (current settings) and a TEST config and aggregates per-variant frame timing. Action-dispatched. status: {enabled,usingTestConfig,interval,hasCachedSnapshots}. start: Enable(), optional interval (seconds); pass manual=true to suppress the timer and drive swaps yourself with action=switch (align swaps to whole benchmark passes — start manual, replay path under A, switch, replay under B, diff). switch: swap USER<->TEST now (manual sessions). stop: Disable(), snapshots retained. clear: ClearCachedSnapshots(). diff: per-key config diff {path,userValue,testValue}. results: the benchmark output — aggregated per-variant per-draw-call timing {hasResults,totalFrames,totalDurationSec,drawCalls:[{label,shaderType,meanA,meanB,delta,...}]}. Authoring the TEST config lives in the Performance Overlay UI.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["status","start","stop","clear","diff","switch","results"]},"interval":{"type":"number"},"manual":{"type":"boolean"}},"required":["action"]}})";
		dvb->RegisterTool("openshaders.abtest", abtestDesc, &AbtestToolHandler, nullptr);

		static constexpr const char* settingsDesc =
			R"({"description":"Save, load, or reset the GLOBAL Open Shaders user configuration (Data/SKSE/Plugins/CommunityShaders/*.json). Action-dispatched, all fire-and-forget on the main thread. save: persist current settings (State::Save). load: re-read settings from disk and apply (State::Load). reset: restore every feature to its defaults then persist. Use after openshaders.feature set/reset to make changes durable, or to roll an A/B session back to the saved baseline.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["save","load","reset"]}},"required":["action"]}})";
		dvb->RegisterTool("openshaders.settings", settingsDesc, &SettingsToolHandler, nullptr);
	}
}

#else

namespace DevBenchBridge
{
	void Install() {}  // inert until built with DEVBENCH_BRIDGE_ENABLED
}

#endif
