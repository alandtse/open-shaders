#include "DevBenchBridge.h"

// Registers our tools into the devbench test bench over its C-ABI. Gated by
// DEVBENCH_BRIDGE_ENABLED (set by CMake when the devbench-api port is available);
// otherwise this file compiles to an empty Install(). Inert at runtime when no
// devbench plugin is present (GetDevBenchInterface001() returns null).

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Feature.h"

#	include <DevBenchAPI.h>
#	include <nlohmann/json.hpp>

#	include <stdexcept>

namespace
{
	// Build the `feature` tool result. May throw (json type errors etc.); the C-ABI
	// handler below contains every exception so none crosses the DLL boundary.
	nlohmann::json BuildFeatureResult(const nlohmann::json& a_args)
	{
		using json = nlohmann::json;
		const std::string action = a_args.value("action", std::string("list"));

		if (action == "list") {
			json out = json::array();
			for (auto* f : Feature::GetFeatureList()) {
				out.push_back(json{
					{ "name", f->GetName() },
					{ "shortName", f->GetShortName() },
					{ "loaded", f->loaded },
					{ "isCore", f->IsCore() },
					{ "supportsVR", f->SupportsVR() },
				});
			}
			return out;
		}
		if (action == "toggle") {
			const std::string shortName = a_args.value("shortName", std::string{});
			// Match over the full feature list (NOT Feature::FindFeatureByShortName,
			// which only matches *loaded* features — that makes toggle one-way: you could
			// disable a feature but never re-enable it). Mirrors the 'list' branch.
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
			const bool desired = a_args.value("enabled", !target->loaded);
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				return json{ { "error", "SKSE task interface unavailable" }, { "shortName", shortName } };
			// Apply on the main thread, then emit a namespaced event so listeners (and a
			// benchmark scenario) can observe the change. EmitEvent accepts any topic; the
			// `openshaders.` prefix marks origin in devbench's shared bus.
			task->AddTask([target, desired, shortName]() {
				target->loaded = desired;
				if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
					const std::string payload = json{ { "shortName", shortName }, { "enabled", desired } }.dump();
					dvb->EmitEvent("openshaders.feature.changed", payload.c_str());
				}
			});
			return json{ { "queued", true }, { "shortName", shortName }, { "requested", desired } };
		}
		return json{ { "error", "unknown action (list|toggle)" }, { "action", action } };
	}

	// CS `feature` tool, re-exposed through devbench. Captureless C function (the C-ABI
	// forbids std::function across the DLL boundary). Runs on devbench's listener thread;
	// mutations marshal to the main thread via SKSE's TaskInterface. Every exception is
	// contained here and a_write is always called once — nothing escapes across the C ABI.
	void FeatureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		using json = nlohmann::json;
		json out;
		try {
			json args = json::object();
			if (a_argsJson && *a_argsJson)
				args = json::parse(a_argsJson);  // throws on malformed input
			if (!args.is_object())
				throw std::runtime_error("arguments must be a JSON object");
			out = BuildFeatureResult(args);
		} catch (const std::exception& e) {
			out = json{ { "error", "invalid request" }, { "detail", e.what() } };
		} catch (...) {
			out = json{ { "error", "unknown handler error" } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
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

		// Namespaced tool name — devbench's registry is shared across plugins, so a bare
		// "feature" could collide with devbench's own or another mod's tool.
		static constexpr const char* featureDesc =
			R"({"description":"List or toggle Open Shaders features.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["list","toggle"]},"shortName":{"type":"string"},"enabled":{"type":"boolean"}}}})";
		dvb->RegisterTool("openshaders.feature", featureDesc, &FeatureToolHandler, nullptr);

		// Further CS tools (feature set/reset, shader-cache, capture, abtest) and
		// shader-recompile events register here via the same dvb->RegisterTool / EmitEvent.
	}
}

#else

namespace DevBenchBridge
{
	void Install() {}  // inert until built with DEVBENCH_BRIDGE_ENABLED
}

#endif
