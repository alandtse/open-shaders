// Remote Control feature: hosts an in-process Model Context Protocol (MCP)
// server inside CommunityShaders.dll, letting AI assistants query and mutate
// runtime state for A/B testing. Off by default and loopback-only.
//
// Transport: HTTP+SSE (Streamable HTTP, MCP 2025-03-26).
// Endpoint:  http://<bind>:<port>/mcp   (modern, single endpoint)
//            http://<bind>:<port>/sse   (legacy SSE, also exposed by cpp-mcp)

#include "Features/RemoteControl.h"

#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/RenderDoc.h"
#include "Features/ScreenshotFeature.h"
#include "Globals.h"
#include "State.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <vector>

// cpp-mcp headers. Kept inside the .cpp only so the vendored httplib/json
// in extern/cpp-mcp/common don't leak into other translation units.
#include "mcp_server.h"
#include "mcp_tool.h"

namespace
{
	// The control endpoint is intentionally loopback-only — exposing it off-host
	// would let any networked client toggle features and dispatch captures.
	bool IsLoopbackAddress(const std::string& host)
	{
		return host == "127.0.0.1" || host == "::1" || host == "localhost";
	}

	void NormalizeBindAddress(std::string& host)
	{
		if (!IsLoopbackAddress(host)) {
			logger::warn("Remote Control: non-loopback bindAddress '{}' rejected; forcing 127.0.0.1", host);
			host = "127.0.0.1";
		}
	}

	int ClampPort(int port)
	{
		return std::clamp(port, 1024, 65535);
	}
}

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
	settings.port = ClampPort(o_json.value("port", 8910));
	settings.bindAddress = o_json.value("bindAddress", std::string("127.0.0.1"));
	NormalizeBindAddress(settings.bindAddress);
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
		"features, and trigger captures. Off by default. The endpoint is "
		"loopback-only — any non-loopback bind address is rejected at load "
		"and bind time.");
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

	ImGui::Separator();
	DrawClientsTable();
}

void RemoteControl::DrawClientsTable()
{
	// Snapshot under the lock to keep the listener-thread updates from
	// racing the draw. The snapshot is small (a handful of sessions at most).
	std::vector<SessionInfo> rows;
	{
		std::lock_guard<std::mutex> lock(sessionMutex);
		rows.reserve(sessions.size());
		for (const auto& [_, info] : sessions) {
			rows.push_back(info);
		}
	}

	const std::string headerLabel = std::format("Connected clients ({})##rc-clients", rows.size());
	if (!ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}

	if (!IsRunning()) {
		ImGui::TextDisabled("Server not running.");
		return;
	}
	if (rows.empty()) {
		ImGui::TextDisabled(
			"No clients connected. Paste the config above into "
			"your MCP host and run a tool to populate this table.");
		return;
	}

	constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
	                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
	                                  ImGuiTableFlags_SortMulti | ImGuiTableFlags_ScrollY;

	enum ColumnId : ImGuiID
	{
		ColSession = 0,
		ColConnected,
		ColIdle,
		ColRequests,
		ColLastTool,
	};

	if (ImGui::BeginTable("##rc-clients-table", 5, flags, ImVec2(0.0f, 120.0f))) {
		ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_DefaultSort, 0.0f, ColSession);
		ImGui::TableSetupColumn("Connected", 0, 0.0f, ColConnected);
		ImGui::TableSetupColumn("Idle for", 0, 0.0f, ColIdle);
		ImGui::TableSetupColumn("Requests", 0, 0.0f, ColRequests);
		ImGui::TableSetupColumn("Last tool", 0, 0.0f, ColLastTool);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		if (auto* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0) {
			std::sort(rows.begin(), rows.end(),
				[&](const SessionInfo& a, const SessionInfo& b) {
					for (int i = 0; i < sortSpecs->SpecsCount; ++i) {
						const auto& spec = sortSpecs->Specs[i];
						const bool desc = spec.SortDirection == ImGuiSortDirection_Descending;
						int cmp = 0;
						switch (static_cast<ColumnId>(spec.ColumnUserID)) {
						case ColSession:
							cmp = a.id.compare(b.id);
							break;
						case ColConnected:
							cmp = a.connected < b.connected ? -1 : (a.connected > b.connected ? 1 : 0);
							break;
						case ColIdle:
							cmp = a.lastSeen < b.lastSeen ? -1 : (a.lastSeen > b.lastSeen ? 1 : 0);
							break;
						case ColRequests:
							cmp = a.requestCount < b.requestCount ? -1 : (a.requestCount > b.requestCount ? 1 : 0);
							break;
						case ColLastTool:
							cmp = a.lastTool.compare(b.lastTool);
							break;
						}
						if (cmp != 0) {
							return desc ? cmp > 0 : cmp < 0;
						}
					}
					return false;
				});
		}

		const auto now = std::chrono::system_clock::now();
		const auto formatRelative = [](std::chrono::seconds sec) -> std::string {
			const auto s = sec.count();
			if (s < 60) {
				return std::format("{}s ago", s);
			}
			if (s < 3600) {
				return std::format("{}m {}s ago", s / 60, s % 60);
			}
			return std::format("{}h {}m ago", s / 3600, (s % 3600) / 60);
		};

		for (const auto& info : rows) {
			ImGui::TableNextRow();
			const auto connectedSec = std::chrono::duration_cast<std::chrono::seconds>(now - info.connected);
			const auto idleSec = std::chrono::duration_cast<std::chrono::seconds>(now - info.lastSeen);

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(info.id.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(formatRelative(connectedSec).c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(formatRelative(idleSec).c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%llu", static_cast<unsigned long long>(info.requestCount));
			ImGui::TableSetColumnIndex(4);
			ImGui::TextUnformatted(info.lastTool.empty() ? "(none)" : info.lastTool.c_str());
		}

		ImGui::EndTable();
	}

	ImGui::TextDisabled(
		"To force-disconnect all clients, toggle 'Enable MCP server' off and back on. "
		"Per-session kick is not exposed by cpp-mcp's public API.");
}

std::string RemoteControl::BuildClientConfig() const
{
	// Streamable HTTP transport per the MCP 2025-03-26 spec. Same shape works
	// for Claude Code, Cursor, Continue, and other MCP hosts.
	// IPv6 literals must be bracketed in a URL authority (RFC 3986 §3.2.2),
	// so the IPv6 loopback "::1" becomes "[::1]". IPv4 / hostnames pass
	// through verbatim.
	const std::string hostInUrl = (settings.bindAddress.find(':') != std::string::npos) ? "[" + settings.bindAddress + "]" : settings.bindAddress;
	const json cfg = {
		{ "mcpServers",
			{ { "community-shaders",
				{
					{ "type", "http" },
					{ "url", std::format("http://{}:{}/mcp",
								 hostInUrl, settings.port) },
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
		// Re-validate at bind time — settings may have been touched via the UI
		// or hot-reload since LoadSettings ran.
		NormalizeBindAddress(settings.bindAddress);
		settings.port = ClampPort(settings.port);

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

		// Drop a session from the bookkeeping map on disconnect. cpp-mcp
		// dispatches this from its listener thread when the SSE/HTTP
		// connection tears down.
		server->register_session_cleanup("remote-control-session-tracker",
			[this](const std::string& sessionId) {
				DropSession(sessionId);
			});

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
	{
		std::lock_guard<std::mutex> lock(sessionMutex);
		sessions.clear();
	}
	logger::info("Remote Control: MCP server stopped");
}

void RemoteControl::RecordToolCall(const std::string& sessionId, const std::string& toolName)
{
	const auto now = std::chrono::system_clock::now();
	std::lock_guard<std::mutex> lock(sessionMutex);
	auto& info = sessions[sessionId];
	if (info.requestCount == 0) {
		info.id = sessionId;
		info.connected = now;
	}
	info.lastSeen = now;
	info.requestCount += 1;
	info.lastTool = toolName;
}

void RemoteControl::DropSession(const std::string& sessionId)
{
	std::lock_guard<std::mutex> lock(sessionMutex);
	sessions.erase(sessionId);
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
	// Five tools, each semantically rich. Reads vs writes vs lifecycles are
	// separated by tool; within each tool, kind/action discriminates the
	// specific operation. See agentic-renderdoc's "Why this design" notes —
	// fewer rich tools outperform expansive suites because the agent reads
	// fewer descriptions and each description carries the operational
	// expertise (timing, gotchas, verification routes).
	RegisterInspectTool();  // reads (non-feature engine state)
	RegisterFeatureTool();  // all feature ops (list/get/set/reset/toggle)
	RegisterConsoleTool();  // Skyrim console passthrough
	RegisterCaptureTool();  // frame capture (renderdoc/screenshot)
	RegisterAbtestTool();   // A/B test lifecycle
}

// Helper used by both inspect(kind="state") and (potentially) future tools.
static mcp::json EngineStateBlob()
{
	const uint frames = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
	const bool vr = REL::Module::IsVR();
	return mcp::json({
		{ "plugin", "CommunityShaders" },
		{ "frame_count", frames },
		{ "vr", vr },
	});
}

// Helper used by feature(action="list") to build one entry per feature.
static mcp::json FeatureEntry(Feature* f)
{
	return mcp::json({
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

void RemoteControl::RegisterInspectTool()
{
	// Single read endpoint for non-feature engine state. Kind-discriminated
	// so future engine reads (weather, cell, player, render targets) extend
	// the same tool rather than spawning new top-level reads. For feature
	// reads (list, get settings), use the `feature` tool with the
	// corresponding action.
	const auto tool = mcp::tool_builder("inspect")
	                      .with_description(
							  "Read non-feature engine state. Kind-dispatched; the "
							  "response is always a JSON object delivered as the "
							  "text of a single content item.\n\n"
							  "Kinds:\n"
							  "  state — { plugin, frame_count, vr }. Frame counter "
							  "monotonically increases each render tick; use as a "
							  "ground truth for verifying that deferred operations "
							  "(see `console`) have had time to run.\n\n"
							  "For feature reads (enumerate / settings), use the "
							  "`feature` tool with action='list' or 'get'.")
	                      .with_string_param("kind",
							  "Currently 'state'. New kinds will be added here "
							  "rather than as new tools.")
	                      .build();
	server->register_tool(tool,
		[this](const mcp::json& params, const std::string& session_id) -> mcp::json {
			RecordToolCall(session_id, "inspect");
			const std::string kind = params.value("kind", std::string{});
			if (kind.empty()) {
				return ErrorResult("missing required parameter 'kind'");
			}
			if (kind == "state") {
				return TextResult(EngineStateBlob().dump());
			}
			return ErrorResult("unknown kind",
				{ { "kind", kind },
					{ "supported", mcp::json::array({ "state" }) } });
		});
}

void RemoteControl::RegisterFeatureTool()
{
	// One tool for all graphics-feature operations. Action-dispatched so the
	// agent has a single description that documents the full feature
	// vocabulary plus the gotchas across all five operations (silent no-op
	// for missing overrides, listener-thread caveats, etc).
	const auto tool = mcp::tool_builder("feature")
	                      .with_description(
							  "All graphics-feature operations — enumerate, "
							  "inspect settings, mutate settings, restore defaults, "
							  "toggle on/off. Action-dispatched; each action takes "
							  "the parameters listed below.\n\n"
							  "Actions:\n"
							  "  list   — no other params. Returns a JSON array; "
							  "each entry has { name, shortName, loaded, version, "
							  "category, isCore, supportsVR, inMenu }.\n"
							  "  get    — params: shortName. Returns the "
							  "Feature::SaveSettings(json) blob. May return null "
							  "if the feature has no SaveSettings/LoadSettings "
							  "override (e.g. LightLimitFix); set/reset will "
							  "silently no-op for these.\n"
							  "  set    — params: shortName, settings (object). "
							  "Calls Feature::LoadSettings on the listener thread. "
							  "Safe for value-assigning LoadSettings (the common "
							  "case) and for features that flip a recompileFlag "
							  "(ScreenSpaceGI, DynamicCubemaps) — the render loop "
							  "picks them up on the next frame. Settings that "
							  "synchronously rebuild GPU resources would race; "
							  "none in-tree currently do.\n"
							  "  reset  — params: shortName. Calls "
							  "Feature::RestoreDefaultSettings(). Distinct from "
							  "set({}) because RestoreDefaultSettings is "
							  "feature-specific reset logic (may release/recreate "
							  "state).\n"
							  "  toggle — params: shortName, enabled (boolean). "
							  "Flips Feature::loaded. Disabled features are "
							  "skipped by ForEachLoadedFeature so their per-frame "
							  "rendering work doesn't run. GPU resources allocated "
							  "in SetupResources are NOT freed — A/B perf/quality, "
							  "not memory reclaim.\n\n"
							  "A/B testing pattern:\n"
							  "  1. feature(action='get', shortName='Skylighting')   → snapshot\n"
							  "  2. feature(action='reset', shortName='Skylighting') → defaults\n"
							  "  3. capture + tracy capture                          → measure\n"
							  "  4. feature(action='set', shortName='Skylighting', settings=<snapshot>) → restore\n\n"
							  "Gotchas:\n"
							  "  • Some features have no SaveSettings/LoadSettings "
							  "override. `get` returns null; `set` and `reset` "
							  "claim success but don't change anything. Confirmed "
							  "case: LightLimitFix.\n"
							  "  • toggle keeps GPU resources alive. If a feature "
							  "still affects rendering after `enabled=false`, it "
							  "has a hook that isn't gated on `loaded` — file an "
							  "issue with the shortName.")
	                      .with_string_param("action",
							  "One of: 'list', 'get', 'set', 'reset', 'toggle'.")
	                      .with_string_param("shortName",
							  "Required for all actions except 'list'. From the "
							  "list response.",
							  /*required=*/false)
	                      .with_object_param("settings",
							  "Required for action='set'. Shape that matches what "
							  "action='get' returned for the same feature.",
							  mcp::json::object(),
							  /*required=*/false)
	                      .with_boolean_param("enabled",
							  "Required for action='toggle'.",
							  /*required=*/false)
	                      .build();
	server->register_tool(tool,
		[this](const mcp::json& params, const std::string& session_id) -> mcp::json {
			RecordToolCall(session_id, "feature");
			const std::string action = params.value("action", std::string{});
			if (action.empty()) {
				return ErrorResult("missing required parameter 'action'");
			}

			if (action == "list") {
				mcp::json features = mcp::json::array();
				for (auto* f : Feature::GetFeatureList()) {
					features.push_back(FeatureEntry(f));
				}
				return TextResult(features.dump());
			}

			const std::string shortName = params.value("shortName", std::string{});
			if (shortName.empty()) {
				return ErrorResult("missing required parameter 'shortName'",
					{ { "action", action } });
			}

			if (action == "toggle") {
				if (!params.contains("enabled") || !params["enabled"].is_boolean()) {
					return ErrorResult("missing required boolean parameter 'enabled'");
				}
				const bool desired = params["enabled"].get<bool>();
				// FindFeatureByShortName filters on loaded==true so it can't
				// help re-enable; walk the full list ourselves.
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
				logger::info("Remote Control: feature(toggle, {}, {}) (was {})",
					shortName, desired, previous);
				return TextResult(mcp::json({
												{ "action", "toggle" },
												{ "shortName", shortName },
												{ "previous", previous },
												{ "current", desired },
											})
						.dump());
			}

			auto* feature = Feature::FindFeatureByShortName(shortName);
			if (!feature) {
				return ErrorResult("feature not found or not loaded",
					{ { "shortName", shortName } });
			}

			if (action == "get") {
				// SaveSettings uses nlohmann::json (unordered). Keep the
				// intermediate value as plain json and dump as a string so
				// we don't have to round-trip through mcp::json's ordered map.
				::json blob;
				feature->SaveSettings(blob);
				return TextResult(blob.dump());
			}
			if (action == "set") {
				if (!params.contains("settings") || !params["settings"].is_object()) {
					return ErrorResult("missing required object parameter 'settings'");
				}
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
				logger::info("Remote Control: feature(set, {})", shortName);
				return TextResult(mcp::json({
												{ "action", "set" },
												{ "shortName", shortName },
												{ "applied", true },
											})
						.dump());
			}
			if (action == "reset") {
				try {
					feature->RestoreDefaultSettings();
				} catch (const std::exception& e) {
					return ErrorResult("RestoreDefaultSettings threw",
						{ { "shortName", shortName }, { "detail", e.what() } });
				}
				logger::info("Remote Control: feature(reset, {})", shortName);
				return TextResult(mcp::json({
												{ "action", "reset" },
												{ "shortName", shortName },
												{ "reset", true },
											})
						.dump());
			}

			return ErrorResult("unknown action",
				{ { "action", action },
					{ "supported", mcp::json::array({ "list", "get", "set", "reset", "toggle" }) } });
		});
}

void RemoteControl::RegisterAbtestTool()
{
	// Single tool for the entire A/B testing lifecycle. Action-dispatched
	// rather than spawning start_abtest / stop_abtest / get_abtest_results
	// / clear_abtest_snapshots / set_abtest_interval — fewer richer tools.
	const auto tool = mcp::tool_builder("abtest")
	                      .with_description(
							  "Drive the built-in A/B testing harness "
							  "(features/Performance Overlay/ABTesting). The "
							  "harness rotates between a USER configuration "
							  "(your current settings) and a TEST configuration "
							  "(typically a preset under test) on a fixed "
							  "interval, snapshots both in memory to avoid disk "
							  "I/O during swaps, and aggregates per-variant "
							  "frame timing so you can compare quality and perf.\n\n"
							  "Actions:\n"
							  "  status   — return enabled, usingTestConfig, "
							  "interval, hasCachedSnapshots.\n"
							  "  start    — Enable() the manager (begin rotating). "
							  "Optional `interval` parameter (seconds) is applied "
							  "first if provided.\n"
							  "  stop     — Disable() the manager. Snapshots are "
							  "retained.\n"
							  "  clear    — ClearCachedSnapshots(). Use to reset "
							  "before a fresh comparison.\n"
							  "  diff     — return the per-key diff list "
							  "(GetConfigDiffEntries) so callers know which "
							  "settings the rotation is actually toggling.\n\n"
							  "Setup of the TEST config itself lives in the "
							  "Performance Overlay UI — this tool only drives "
							  "the lifecycle, not the test-config authoring.")
	                      .with_string_param("action",
							  "'status', 'start', 'stop', 'clear', or 'diff'.")
	                      .with_number_param("interval",
							  "Seconds per variant when action='start'. "
							  "Default 0 (no change).",
							  /*required=*/false)
	                      .build();
	server->register_tool(tool,
		[this](const mcp::json& params, const std::string& session_id) -> mcp::json {
			RecordToolCall(session_id, "abtest");
			const std::string action = params.value("action", std::string{});
			if (action.empty()) {
				return ErrorResult("missing required parameter 'action'");
			}
			auto* mgr = ABTestingManager::GetSingleton();
			if (!mgr) {
				return ErrorResult("ABTestingManager singleton unavailable");
			}

			const auto statusBlob = [&]() {
				return mcp::json({
					{ "enabled", mgr->IsEnabled() },
					{ "usingTestConfig", mgr->IsUsingTestConfig() },
					{ "interval", mgr->GetTestInterval() },
					{ "hasCachedSnapshots", mgr->HasCachedSnapshots() },
				});
			};

			if (action == "status") {
				return TextResult(statusBlob().dump());
			}
			if (action == "start") {
				if (params.contains("interval") && params["interval"].is_number()) {
					const auto secs = params["interval"].get<int>();
					if (secs > 0) {
						mgr->SetTestInterval(static_cast<uint32_t>(secs));
					}
				}
				mgr->Enable();
				logger::info("Remote Control: abtest(start)");
				return TextResult(statusBlob().dump());
			}
			if (action == "stop") {
				mgr->Disable();
				logger::info("Remote Control: abtest(stop)");
				return TextResult(statusBlob().dump());
			}
			if (action == "clear") {
				mgr->ClearCachedSnapshots();
				logger::info("Remote Control: abtest(clear)");
				return TextResult(statusBlob().dump());
			}
			if (action == "diff") {
				mcp::json entries = mcp::json::array();
				for (const auto& entry : mgr->GetConfigDiffEntries()) {
					// SettingsDiffEntry uses generic a/b labels (see
					// Utils/FileSystem.h). For A/B testing semantics here,
					// `a` is USER and `b` is TEST.
					entries.push_back({
						{ "path", entry.path },
						{ "userValue", entry.aValue },
						{ "testValue", entry.bValue },
					});
				}
				return TextResult(mcp::json({
												{ "hasCachedSnapshots", mgr->HasCachedSnapshots() },
												{ "entries", std::move(entries) },
											})
						.dump());
			}
			return ErrorResult("unknown action",
				{ { "action", action },
					{ "supported", mcp::json::array({ "status", "start", "stop", "clear", "diff" }) } });
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
		[this](const mcp::json& params, const std::string& session_id) -> mcp::json {
			RecordToolCall(session_id, "capture");
			const std::string kind = params.value("kind", std::string{});
			if (kind.empty()) {
				return ErrorResult("missing required parameter 'kind'");
			}
			const uint enqueuedFrame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;

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
							  "To verify a state change, poll inspect(kind='state') "
							  "until frame_count > enqueued_at_frame (at least one tick "
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
		[this](const mcp::json& params, const std::string& session_id) -> mcp::json {
			RecordToolCall(session_id, "console");
			std::string command = params.value("command", std::string{});
			if (command.empty()) {
				return ErrorResult("missing required parameter 'command'");
			}
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return ErrorResult("SKSE TaskInterface unavailable");
			}
			const uint enqueuedFrame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
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
