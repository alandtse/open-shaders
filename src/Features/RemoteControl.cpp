// Remote Control: status panel for the devbench bridge.
//
// CS used to embed its own Model Context Protocol server here. That server has
// been removed — its tools now register into the external devbench host (the
// devbench SKSE plugin, https://github.com/alandtse/devbench) via DevBenchBridge
// (src/DevBenchBridge.cpp), exposed over both MCP and REST. This feature is now a
// read-only panel: it reports whether devbench is present, what CS registered, and
// the port devbench bound, so users can confirm the integration without leaving the
// game.

#include "Features/RemoteControl.h"

#include "DevBenchBridge.h"
#include "Globals.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <DevBenchAPI.h>
#endif

namespace
{
	// devbench writes the host port it bound to here on startup. We only read it for
	// display; the bridge itself talks to devbench in-process via the C-ABI, not the port.
	constexpr const char* kRuntimeJsonPath = "Data/SKSE/Plugins/devbench/runtime.json";

	// Returns the bound port from devbench's runtime.json, or 0 if absent/unreadable.
	int ReadDevBenchPort()
	{
		std::error_code ec;
		if (!std::filesystem::exists(kRuntimeJsonPath, ec))
			return 0;
		try {
			std::ifstream in(kRuntimeJsonPath);
			if (!in)
				return 0;
			json runtime = json::parse(in, nullptr, /*allow_exceptions=*/false);
			if (runtime.is_discarded() || !runtime.is_object())
				return 0;
			return runtime.value("port", 0);
		} catch (...) {
			return 0;  // malformed runtime.json is non-fatal — just hide the port
		}
	}
}

RemoteControl* RemoteControl::GetSingleton()
{
	return &globals::features::remoteControl;
}

void RemoteControl::Load()
{
	// Register CS's tools into the devbench host if one is present. DevBenchBridge::Install
	// is idempotent on the devbench side (re-registering replaces) and a no-op when no host
	// is present or the bridge was built disabled, so calling it here is safe even though
	// XSEPlugin's kDataLoaded also calls it — whichever runs first wins, the other is inert.
	DevBenchBridge::Install();
}

void RemoteControl::Reset()
{
	// No per-frame state.
}

void RemoteControl::LoadSettings(json&)
{
	// No configurable settings — the bridge has no server, port, or bind address.
}

void RemoteControl::SaveSettings(json&)
{
	// No configurable settings.
}

void RemoteControl::RestoreDefaultSettings()
{
	// No configurable settings.
}

void RemoteControl::DrawSettings()
{
	ImGui::TextWrapped(
		"Open Shaders registers its tools into the external devbench host so AI "
		"assistants (Claude Code, Cursor, etc.) can drive A/B testing, toggle "
		"features, inspect engine state, trigger captures, and save/load settings "
		"over MCP and REST. There is no in-game server — install the devbench SKSE "
		"plugin to enable the integration.");
	ImGui::Spacing();

#ifdef DEVBENCH_BRIDGE_ENABLED
	auto* dvb = DevBenchAPI::GetDevBenchInterface001();
	if (dvb) {
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
			"devbench host present (build %u)", dvb->GetBuildNumber());

		if (const int port = ReadDevBenchPort(); port > 0) {
			ImGui::Text("Host bound on port %d (from %s)", port, kRuntimeJsonPath);
		} else {
			ImGui::TextDisabled(
				"Port unknown — devbench writes it to %s once it binds.",
				kRuntimeJsonPath);
		}
	} else {
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
			"devbench host not detected. Install the devbench SKSE plugin; "
			"Open Shaders' tools register automatically once it is present.");
	}

	ImGui::Separator();
	ImGui::TextUnformatted("Tools Open Shaders exposes through devbench:");
	ImGui::BulletText("openshaders.feature — list / get / set / reset / toggle features");
	ImGui::BulletText("openshaders.inspect — engine state and shader-cache status");
	ImGui::BulletText("openshaders.shadercache — clear / delete the compiled cache");
	ImGui::BulletText("openshaders.capture — RenderDoc / screenshot capture");
	ImGui::BulletText("openshaders.abtest — A/B test lifecycle");
	ImGui::BulletText("openshaders.settings — save / load / reset the global config");
	ImGui::TextDisabled(
		"Note: the console tool is provided by devbench itself, not Open Shaders.");
#else
	ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
		"This build was compiled without the devbench bridge "
		"(DEVBENCH_BRIDGE=OFF). No tools are registered.");
#endif
}
