#include "DevBenchBridge.h"

// Activation (when devbench parity work is ready):
//   1. Add "devbench-api" to vcpkg.json dependencies, with the devbench repo's
//      cmake/ports on VCPKG_OVERLAY_PORTS (local checkout until devbench is published;
//      then a pinned REF/SHA512 in the port's portfile).
//   2. In CMake: find_package(devbench-api CONFIG REQUIRED);
//      target_link_libraries(CommunityShaders PRIVATE DevBench::API);
//      target_compile_definitions(CommunityShaders PRIVATE DEVBENCH_BRIDGE_ENABLED).
//   3. Build + runtime-test the two-plugin path (CS tools appear in devbench /api/tools).
// Until step 2 defines DEVBENCH_BRIDGE_ENABLED, this file compiles to an empty Install().

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Feature.h"

#	include <DevBenchAPI.h>
#	include <nlohmann/json.hpp>

namespace
{
	// CS `feature` tool, re-exposed through devbench. Captureless C function (the C-ABI
	// forbids std::function across the DLL boundary). Runs on devbench's listener thread;
	// mutations marshal to the main thread via SKSE's TaskInterface, as RemoteControl does.
	void FeatureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		using json = nlohmann::json;
		json args = json::object();
		if (a_argsJson && *a_argsJson) {
			try {
				args = json::parse(a_argsJson);
			} catch (...) {
			}
		}
		const std::string action = args.value("action", std::string("list"));
		json out;

		if (action == "list") {
			out = json::array();
			for (auto* f : Feature::GetFeatureList()) {
				out.push_back(json{
					{ "name", f->GetName() },
					{ "shortName", f->GetShortName() },
					{ "loaded", f->loaded },
					{ "isCore", f->IsCore() },
					{ "supportsVR", f->SupportsVR() },
				});
			}
		} else if (action == "toggle") {
			const std::string shortName = args.value("shortName", std::string{});
			auto* target = shortName.empty() ? nullptr : Feature::FindFeatureByShortName(shortName);
			if (!target) {
				out = json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			} else {
				const bool desired = args.value("enabled", !target->loaded);
				if (auto* task = SKSE::GetTaskInterface())
					task->AddTask([target, desired]() { target->loaded = desired; });
				out = json{ { "queued", true }, { "shortName", shortName }, { "requested", desired } };
			}
		} else {
			out = json{ { "error", "unknown action (list|toggle)" }, { "action", action } };
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

		static constexpr const char* featureDesc =
			R"({"description":"List or toggle Community Shaders features.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["list","toggle"]},"shortName":{"type":"string"},"enabled":{"type":"boolean"}}}})";
		dvb->RegisterTool("feature", featureDesc, &FeatureToolHandler, nullptr);

		// TODO(parity): feature set/reset, inspect(shadercache), capture
		// (renderdoc/screenshot), abtest, and publish shader-recompile events via
		// dvb->EmitEvent — porting the remaining RemoteControl tools, then retire CS's
		// built-in server.
	}
}

#else

namespace DevBenchBridge
{
	void Install() {}  // inert until built with DEVBENCH_BRIDGE_ENABLED
}

#endif
