// Remote Control feature: hosts an in-process Model Context Protocol (MCP)
// server inside CommunityShaders.dll, letting AI assistants query and mutate
// runtime state for A/B testing. Off by default and loopback-only.
//
// Transport: HTTP+SSE (Streamable HTTP, MCP 2025-03-26).
// Endpoint:  http://<bind>:<port>/mcp   (modern, single endpoint)
//            http://<bind>:<port>/sse   (legacy SSE, also exposed by cpp-mcp)

#include "Features/RemoteControl.h"

#include "Globals.h"
#include "State.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <format>
#include <stdexcept>

// cpp-mcp headers. Kept inside the .cpp only so the vendored httplib/json
// in extern/cpp-mcp/common don't leak into other translation units.
#include "mcp_server.h"
#include "mcp_tool.h"

RemoteControl* RemoteControl::GetSingleton()
{
	return &globals::features::remoteControl;
}

RemoteControl::RemoteControl() = default;

RemoteControl::~RemoteControl()
{
	StopServer();
}

void RemoteControl::Load()
{
	// Settings have already been read in by the time Load() fires.
	if (settings.enabled) {
		StartServer();
	}
}

void RemoteControl::Reset()
{
	// No per-frame state to reset.
}

void RemoteControl::LoadSettings(json& o_json)
{
	settings.enabled = o_json.value("enabled", false);
	settings.port = o_json.value("port", 8910);
	settings.bindAddress = o_json.value("bindAddress", std::string("127.0.0.1"));
}

void RemoteControl::SaveSettings(json& o_json)
{
	o_json["enabled"] = settings.enabled;
	o_json["port"] = settings.port;
	o_json["bindAddress"] = settings.bindAddress;
}

void RemoteControl::RestoreDefaultSettings()
{
	settings = Settings{};
}

void RemoteControl::DrawSettings()
{
	ImGui::TextWrapped(
		"Exposes Community Shaders over Model Context Protocol (MCP) so AI "
		"assistants such as Claude Code can drive A/B testing, toggle "
		"features, and trigger captures. Off by default. Bound to 127.0.0.1 "
		"unless explicitly changed.");
	ImGui::Spacing();

	const bool wasEnabled = settings.enabled;
	if (ImGui::Checkbox("Enable MCP server", &settings.enabled)) {
		if (settings.enabled && !wasEnabled) {
			StartServer();
		} else if (!settings.enabled && wasEnabled) {
			StopServer();
		}
	}

	// Port + bind address can only be edited while the server is stopped.
	ImGui::BeginDisabled(IsRunning());
	ImGui::InputInt("Port", &settings.port);
	settings.port = std::clamp(settings.port, 1024, 65535);
	ImGui::InputText("Bind address", &settings.bindAddress);
	ImGui::EndDisabled();
	if (IsRunning()) {
		ImGui::SameLine();
		ImGui::TextDisabled("(stop the server to edit)");
	}

	if (!lastError.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
			"Server error: %s", lastError.c_str());
	}

	if (IsRunning()) {
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
			"Listening on %s:%d", settings.bindAddress.c_str(), activePort);
	}

	ImGui::Separator();
	ImGui::Text("Connect from an MCP client (Claude Code, Cursor, etc.):");

	if (ImGui::Button("Copy MCP client config to clipboard")) {
		ImGui::SetClipboardText(BuildClientConfig().c_str());
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Paste the JSON into your Claude Code settings under "
			"\"mcpServers\". Other MCP hosts (Cursor, Continue) accept the "
			"same shape.");
	}

	if (ImGui::CollapsingHeader("Config preview")) {
		const auto preview = BuildClientConfig();
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted(preview.c_str());
		ImGui::PopTextWrapPos();
	}
}

std::string RemoteControl::BuildClientConfig() const
{
	// Streamable HTTP transport per the MCP 2025-03-26 spec. Same shape works
	// for Claude Code, Cursor, Continue, and other MCP hosts.
	const json cfg = {
		{ "mcpServers",
			{ { "community-shaders",
				{
					{ "type", "http" },
					{ "url", std::format("http://{}:{}/mcp",
								 settings.bindAddress, settings.port) },
				} } } }
	};
	return cfg.dump(4);
}

void RemoteControl::StartServer()
{
	if (server) {
		return;
	}
	lastError.clear();

	try {
		mcp::server::configuration cfg;
		cfg.host = settings.bindAddress;
		cfg.port = settings.port;
		cfg.name = "Community Shaders";
		cfg.version = "0.1.0";

		server = std::make_unique<mcp::server>(cfg);
		server->set_server_info(cfg.name, cfg.version);
		server->set_capabilities({ { "tools", mcp::json::object() } });
		server->set_instructions(
			"This server exposes the Skyrim Community Shaders plugin. "
			"Use the tools to inspect engine state for performance "
			"investigation and A/B testing of graphics features.");

		RegisterTools();

		if (!server->start(false)) {  // false = non-blocking
			throw std::runtime_error("server.start() returned false");
		}
		activePort = settings.port;
		logger::info("Remote Control: MCP server listening on {}:{}",
			settings.bindAddress, activePort);
	} catch (const std::exception& e) {
		lastError = e.what();
		logger::error("Remote Control: failed to start MCP server: {}",
			e.what());
		server.reset();
		activePort = 0;
	}
}

void RemoteControl::StopServer()
{
	if (!server) {
		return;
	}
	try {
		server->stop();
	} catch (...) {
		// best-effort on shutdown
	}
	server.reset();
	activePort = 0;
	logger::info("Remote Control: MCP server stopped");
}

// Helper: wrap a payload string in the MCP tool-result content envelope
// (an array of typed content items). Tools return application data as the
// "text" field of a single content item; consumers typically parse it as
// JSON.
static mcp::json TextResult(std::string text)
{
	return mcp::json::array({ mcp::json{
		{ "type", "text" },
		{ "text", std::move(text) } } });
}

// Helper: emit an error result. Convention: a single text content item
// containing a JSON object with "error" + optional context fields, so
// callers always get parseable JSON whether the call succeeded or not.
static mcp::json ErrorResult(std::string_view message, mcp::json context = {})
{
	mcp::json obj = { { "error", message } };
	if (!context.is_null()) {
		obj.update(context);
	}
	return mcp::json::array({ mcp::json{
		{ "type", "text" },
		{ "text", obj.dump() } } });
}

void RemoteControl::RegisterTools()
{
	RegisterGetStateTool();
	RegisterListFeaturesTool();
	RegisterGetFeatureSettingsTool();
	RegisterToggleFeatureTool();
	RegisterSetFeatureSettingsTool();
	RegisterResetFeatureSettingsTool();
	RegisterConsoleTool();
	RegisterCaptureTool();
}

void RemoteControl::RegisterGetStateTool()
{
	const auto tool = mcp::tool_builder("get_state")
	                      .with_description(
							  "Return Community Shaders runtime state: "
							  "frame counter, plugin version, VR mode.")
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
			const uint frames = globals::state ? globals::state->frameCount : 0;
			const bool vr = REL::Module::IsVR();
			return TextResult(std::format(
				R"({{"frame_count":{},"vr":{},"plugin":"CommunityShaders"}})",
				frames, vr ? "true" : "false"));
		});
}

void RemoteControl::RegisterListFeaturesTool()
{
	const auto tool = mcp::tool_builder("list_features")
	                      .with_description(
							  "Enumerate Community Shaders graphics features. "
							  "Returns a JSON array with one entry per feature: "
							  "name, shortName, loaded, version, category, "
							  "isCore, supportsVR.")
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
			mcp::json features = mcp::json::array();
			for (auto* f : Feature::GetFeatureList()) {
				features.push_back({
					{ "name", f->GetName() },
					{ "shortName", f->GetShortName() },
					{ "loaded", f->loaded },
					{ "version", f->version },
					{ "category", std::string(f->GetCategory()) },
					{ "isCore", f->IsCore() },
					{ "supportsVR", f->SupportsVR() },
					{ "inMenu", f->IsInMenu() },
				});
			}
			return TextResult(features.dump());
		});
}

void RemoteControl::RegisterToggleFeatureTool()
{
	const auto tool = mcp::tool_builder("toggle_feature")
	                      .with_description(
							  "Enable or disable a feature at runtime by "
							  "flipping its 'loaded' flag. Disabled features "
							  "are skipped by Feature::ForEachLoadedFeature so "
							  "their per-frame rendering work doesn't run. GPU "
							  "resources are NOT freed — this is for A/B "
							  "perf/quality comparisons, not memory reclaim. "
							  "Reverting only requires another toggle_feature "
							  "call.")
	                      .with_string_param("shortName",
							  "Feature shortName as returned by list_features.")
	                      .with_boolean_param("enabled",
							  "true to load (run), false to skip.")
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
			const std::string shortName = params.value("shortName", std::string{});
			if (shortName.empty()) {
				return ErrorResult("missing required parameter 'shortName'");
			}
			if (!params.contains("enabled") || !params["enabled"].is_boolean()) {
				return ErrorResult("missing required boolean parameter 'enabled'");
			}
			const bool desired = params["enabled"].get<bool>();
			// FindFeatureByShortName filters on `loaded == true`, so it won't
			// help us re-enable a feature. Walk the full list ourselves.
			Feature* target = nullptr;
			for (auto* f : Feature::GetFeatureList()) {
				if (f->GetShortName() == shortName) {
					target = f;
					break;
				}
			}
			if (!target) {
				return ErrorResult("feature not found",
					{ { "shortName", shortName } });
			}
			const bool previous = target->loaded;
			target->loaded = desired;
			logger::info("Remote Control: toggle_feature({}, {}) (was {})",
				shortName, desired, previous);
			return TextResult(mcp::json({
											{ "shortName", shortName },
											{ "previous", previous },
											{ "current", desired },
										})
					.dump());
		});
}

void RemoteControl::RegisterCaptureTool()
{
	// One tool for all frame-capture kinds, kind-dispatched. Adding new
	// capture types later (e.g. tracy snapshot, video clip) extends this
	// tool's `kind` enum rather than spawning new top-level tools.
	const auto tool = mcp::tool_builder("capture")
	                      .with_description(
							  "Trigger a frame capture on the next render. Kind-"
							  "dispatched so all capture flavors live behind one "
							  "tool — see the agentic-renderdoc design notes.\n\n"
							  "Supported kinds:\n"
							  "  renderdoc  — RenderDoc multi-frame capture via "
							  "the in-application API. Honors the `frames` "
							  "parameter (default 1, max 120). RenderDoc must "
							  "be attached or the in-app DLL loaded; check "
							  "list_features for RenderDoc loaded=true. Output "
							  "lands in RenderDoc's configured captures dir.\n"
							  "  screenshot — Lossless screenshot via the "
							  "Screenshot feature's non-blocking capture path. "
							  "The `frames` parameter is ignored. Output lands "
							  "in the game's Screenshots/ folder.\n\n"
							  "Fire-and-forget: the trigger flag is set "
							  "immediately and the render loop consumes it on "
							  "the next frame. No artifact path is returned "
							  "synchronously — for renderdoc, inspect the "
							  "captures directory; for screenshots, watch the "
							  "Screenshots folder.")
	                      .with_string_param("kind",
							  "'renderdoc' or 'screenshot'.")
	                      .with_number_param("frames",
							  "RenderDoc only: number of consecutive frames to "
							  "capture (1-120). Default 1. Ignored for "
							  "screenshot.",
							  /*required=*/false)
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
			const std::string kind = params.value("kind", std::string{});
			if (kind.empty()) {
				return ErrorResult("missing required parameter 'kind'");
			}
			const uint enqueuedFrame = globals::state ? globals::state->frameCount : 0;

			if (kind == "renderdoc") {
				auto* renderDoc = &globals::features::renderDoc;
				if (!renderDoc->loaded) {
					return ErrorResult("RenderDoc feature is not loaded",
						{ { "hint", "list_features shows RenderDoc.loaded" } });
				}
				if (!renderDoc->IsAvailable()) {
					return ErrorResult(
						"RenderDoc API not available — attach RenderDoc or "
						"load the in-app DLL");
				}
				uint32_t frameCount = 1;
				if (params.contains("frames") && params["frames"].is_number()) {
					const auto raw = params["frames"].get<int>();
					frameCount = static_cast<uint32_t>(std::clamp(raw, 1, 120));
				}
				if (frameCount == 1) {
					renderDoc->TriggerCapture();
				} else {
					renderDoc->TriggerMultiFrameCapture(frameCount);
				}
				logger::info("Remote Control: capture(renderdoc, {}) at frame {}",
					frameCount, enqueuedFrame);
				return TextResult(mcp::json({
												{ "queued", true },
												{ "kind", "renderdoc" },
												{ "frames", frameCount },
												{ "enqueued_at_frame", enqueuedFrame },
											})
						.dump());
			}

			if (kind == "screenshot") {
				auto* shot = &globals::features::screenshotFeature;
				if (!shot->loaded) {
					return ErrorResult("Screenshot feature is not loaded");
				}
				shot->captureRequested.store(true, std::memory_order_release);
				logger::info("Remote Control: capture(screenshot) at frame {}",
					enqueuedFrame);
				return TextResult(mcp::json({
												{ "queued", true },
												{ "kind", "screenshot" },
												{ "enqueued_at_frame", enqueuedFrame },
											})
						.dump());
			}

			return ErrorResult("unknown kind",
				{ { "kind", kind },
					{ "supported", mcp::json::array({ "renderdoc", "screenshot" }) } });
		});
}

void RemoteControl::RegisterConsoleTool()
{
	// Singular tool for the entire console concern. Future console-related
	// capabilities (history readout, command lookup, etc.) get added as
	// optional parameters / additional response fields here rather than as
	// separate tools — per the "fewer, semantically rich tools" philosophy.
	const auto tool = mcp::tool_builder("console")
	                      .with_description(
							  "Execute a Skyrim console command. Fire-and-forget: "
							  "the command is queued onto the main game thread via "
							  "SKSE's TaskInterface and runs on the next tick. "
							  "Returns immediately with the frame counter at the "
							  "moment of enqueue.\n\n"
							  "RE::Console::ExecuteCommand is `void` — there is "
							  "no per-command return value. RE::ConsoleLog is a "
							  "shared sink (engine + every SKSE plugin) with no "
							  "command-to-output correlation, and many useful "
							  "commands are silent (tcl, tfc, tg, tm, tlb…), so "
							  "scraping console output is unreliable and "
							  "intentionally NOT exposed.\n\n"
							  "To verify a state change, poll get_state until "
							  "frame_count > enqueued_at_frame (at least one tick "
							  "elapsed), then observe via side channels: tracy "
							  "captures for perf-affecting changes, "
							  "capture(kind='renderdoc'|'screenshot') for visual "
							  "confirmation, or future feature-specific get_* "
							  "tools that read RE:: state directly.\n\n"
							  "Common A/B-relevant commands:\n"
							  "  tcl                — toggle player collision\n"
							  "  tfc [1]            — free camera (1 = pause game)\n"
							  "  tg                 — toggle grass\n"
							  "  tm                 — toggle menus / HUD\n"
							  "  tll <0..15>        — toggle land LOD level\n"
							  "  setweather <FormID>— force weather (persistent)\n"
							  "  fw <FormID>        — force weather (temporary)\n"
							  "  coc <CellName>     — teleport to cell\n"
							  "  set timescale to N — game-time multiplier\n")
	                      .with_string_param("command",
							  "The console command, exactly as typed after the ~ key.")
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
			std::string command = params.value("command", std::string{});
			if (command.empty()) {
				return ErrorResult("missing required parameter 'command'");
			}
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return ErrorResult("SKSE TaskInterface unavailable");
			}
			const uint enqueuedFrame = globals::state ? globals::state->frameCount : 0;
			// Capture by value so the string outlives this lambda's scope.
			task->AddTask([command]() {
				RE::Console::ExecuteCommand(command.c_str());
			});
			logger::info("Remote Control: console({}) queued at frame {}",
				command, enqueuedFrame);
			return TextResult(mcp::json({
											{ "queued", true },
											{ "command", std::move(command) },
											{ "enqueued_at_frame", enqueuedFrame },
										})
					.dump());
		});
}

void RemoteControl::RegisterResetFeatureSettingsTool()
{
	const auto tool = mcp::tool_builder("reset_feature_settings")
	                      .with_description(
							  "Restore a feature's settings to their built-in "
							  "defaults via Feature::RestoreDefaultSettings(). "
							  "Distinct from set_feature_settings({}) because "
							  "RestoreDefaultSettings is feature-specific reset "
							  "logic (may release/recreate per-feature state in "
							  "ways LoadSettings({}) does not).\n\n"
							  "Useful as the 'B' side of an A/B test: capture "
							  "current settings with get_feature_settings, run "
							  "reset_feature_settings, observe via tracy + a "
							  "renderdoc capture, then call set_feature_settings "
							  "with the captured blob to return to the user's "
							  "configuration. Same listener-thread caveats as "
							  "set_feature_settings.")
	                      .with_string_param("shortName",
							  "Feature shortName as returned by list_features.")
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
			const std::string shortName = params.value("shortName", std::string{});
			if (shortName.empty()) {
				return ErrorResult("missing required parameter 'shortName'");
			}
			auto* feature = Feature::FindFeatureByShortName(shortName);
			if (!feature) {
				return ErrorResult("feature not found or not loaded",
					{ { "shortName", shortName } });
			}
			try {
				feature->RestoreDefaultSettings();
			} catch (const std::exception& e) {
				return ErrorResult("RestoreDefaultSettings threw",
					{ { "shortName", shortName }, { "detail", e.what() } });
			}
			logger::info("Remote Control: reset_feature_settings({})", shortName);
			return TextResult(mcp::json({
											{ "shortName", shortName },
											{ "reset", true },
										})
					.dump());
		});
}

void RemoteControl::RegisterSetFeatureSettingsTool()
{
	// Empty schema for the "settings" object means "any shape" — the actual
	// schema is feature-specific and discoverable via get_feature_settings.
	const auto tool = mcp::tool_builder("set_feature_settings")
	                      .with_description(
							  "Replace a feature's settings with the supplied "
							  "JSON blob. Schema is feature-specific; use "
							  "get_feature_settings to fetch the current "
							  "shape, mutate the fields you care about, then "
							  "send the whole object back. Changes take "
							  "effect on the next frame.\n\n"
							  "CAVEAT: handler runs on the cpp-mcp listener "
							  "thread, NOT the render thread. Settings that "
							  "merely deserialize into member variables are "
							  "safe (the same pattern the ImGui menu uses on "
							  "the input thread). Settings whose LoadSettings "
							  "synchronously rebuilds GPU resources may "
							  "race the renderer — to be tightened with a "
							  "command queue in a follow-up commit.")
	                      .with_string_param("shortName",
							  "Feature shortName as returned by list_features.")
	                      .with_object_param("settings",
							  "Settings blob as returned by get_feature_settings.",
							  mcp::json::object())
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
			const std::string shortName = params.value("shortName", std::string{});
			if (shortName.empty()) {
				return ErrorResult("missing required parameter 'shortName'");
			}
			if (!params.contains("settings") || !params["settings"].is_object()) {
				return ErrorResult("missing required object parameter 'settings'");
			}
			auto* feature = Feature::FindFeatureByShortName(shortName);
			if (!feature) {
				return ErrorResult("feature not found or not loaded",
					{ { "shortName", shortName } });
			}
			// Round-trip through dump/parse to convert from cpp-mcp's
			// ordered_json (params) to the feature's plain nlohmann::json.
			::json blob;
			try {
				blob = ::json::parse(params["settings"].dump());
			} catch (const std::exception& e) {
				return ErrorResult("settings is not valid JSON",
					{ { "detail", e.what() } });
			}
			try {
				feature->LoadSettings(blob);
			} catch (const std::exception& e) {
				return ErrorResult("LoadSettings threw",
					{ { "shortName", shortName }, { "detail", e.what() } });
			}
			logger::info("Remote Control: set_feature_settings({})", shortName);
			return TextResult(mcp::json({
											{ "shortName", shortName },
											{ "applied", true },
										})
					.dump());
		});
}

void RemoteControl::RegisterGetFeatureSettingsTool()
{
	const auto tool = mcp::tool_builder("get_feature_settings")
	                      .with_description(
							  "Return the current JSON settings blob for a "
							  "single feature. Use list_features to discover "
							  "shortNames. The exact schema is feature-specific "
							  "— it mirrors what Feature::SaveSettings emits to "
							  "the on-disk config and what set_feature_settings "
							  "expects back.")
	                      .with_string_param("shortName",
							  "Feature shortName as returned by list_features.")
	                      .build();
	server->register_tool(tool,
		[](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
			const std::string shortName = params.value("shortName", std::string{});
			if (shortName.empty()) {
				return ErrorResult("missing required parameter 'shortName'");
			}
			auto* feature = Feature::FindFeatureByShortName(shortName);
			if (!feature) {
				return ErrorResult("feature not found or not loaded",
					{ { "shortName", shortName } });
			}
			// SaveSettings() uses nlohmann::json (unordered map). Keep the
			// intermediate value as plain json and re-emit as a string so
			// we don't have to round-trip through mcp::json's ordered map.
			::json blob;
			feature->SaveSettings(blob);
			return TextResult(blob.dump());
		});
}
