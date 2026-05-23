#pragma once

#include "Feature.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

// Forward declare cpp-mcp types so we don't leak its vendored
// httplib / json headers into consumers of this header.
namespace mcp
{
	class server;
	struct tool;
	// cpp-mcp's tool_handler is std::function<json(const json&, const std::string&)>
	// where `json` is an alias for ordered_json — that can't be forward-declared
	// cleanly without dragging the full vendored nlohmann/json header into this
	// public header. Tool registration therefore stays in the .cpp where the
	// real signature is in scope; only opaque pointers are exposed here.
}

class RemoteControl : public Feature
{
public:
	static RemoteControl* GetSingleton();

	// Feature overrides — see Feature.h for contracts.
	std::string GetName() override { return "Remote Control"; }
	std::string GetShortName() override { return "RemoteControl"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool IsCore() const override { return true; }
	bool IsInMenu() const override { return true; }
	bool SupportsVR() override { return true; }
	std::string_view GetShaderDefineName() override { return ""; }
	bool HasShaderDefine(RE::BSShader::Type) override { return false; }

	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Expose Community Shaders to AI assistants over Model Context Protocol (MCP).",
			{
				"Loopback-only JSON-RPC server, off by default",
				"Pair with Claude Code / Cursor / Continue for A/B testing",
				"One-click clipboard copy of MCP client config",
			}
		};
	}

	// Lifecycle
	void Load() override;
	void Reset() override;

	// Settings persistence
	void DrawSettings() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

	struct Settings
	{
		bool enabled = false;                   // opt-in
		int port = 8910;                        // arbitrary high port
		std::string bindAddress = "127.0.0.1";  // loopback by default
	} settings;

	RemoteControl();
	~RemoteControl();

	RemoteControl(const RemoteControl&) = delete;
	RemoteControl& operator=(const RemoteControl&) = delete;
	RemoteControl(RemoteControl&&) = delete;
	RemoteControl& operator=(RemoteControl&&) = delete;

	// Session bookkeeping for the ImGui "Connected clients" table.
	// Updated on every tool invocation (listener thread) and on session
	// cleanup (cpp-mcp callback). Read from the main thread when drawing.
	struct SessionInfo
	{
		std::string id;
		std::chrono::system_clock::time_point connected;
		std::chrono::system_clock::time_point lastSeen;
		uint64_t requestCount = 0;
		std::string lastTool;
	};

private:
	void StartServer();
	void StopServer();
	bool IsRunning() const noexcept { return server != nullptr; }
	std::string BuildClientConfig() const;
	void RegisterTools();
	void RegisterInspectTool();
	void RegisterFeatureTool();
	void RegisterConsoleTool();
	void RegisterCaptureTool();
	void RegisterAbtestTool();

	// Records a tool invocation against the per-session table.
	// Safe to call from the cpp-mcp listener thread.
	void RecordToolCall(const std::string& sessionId, const std::string& toolName);
	// Drops a session from the table on disconnect.
	void DropSession(const std::string& sessionId);
	// Draws the connected-clients ImGui table.
	void DrawClientsTable();

	std::unique_ptr<mcp::server> server;
	int activePort = 0;
	std::string lastError;

	mutable std::mutex sessionMutex;
	std::unordered_map<std::string, SessionInfo> sessions;
};
