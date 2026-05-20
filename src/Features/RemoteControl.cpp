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

void RemoteControl::RegisterTools()
{
	RegisterGetStateTool();
	RegisterListFeaturesTool();
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
