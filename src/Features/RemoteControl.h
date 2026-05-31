#pragma once

#include "Feature.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// Status panel for the devbench bridge. CS's tools are registered into the external
// devbench host via DevBenchBridge (see src/DevBenchBridge.cpp); this feature surfaces,
// in the in-game menu, whether devbench is present, what CS registered, and which port
// the host bound. There is no server, port, or bind address to configure here.
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
			"Expose Open Shaders to AI assistants through the external devbench host.",
			{
				"Registers feature, inspect, shadercache, capture, and settings tools",
				"Drivable over MCP and REST from the shared devbench bench",
				"No in-game server — install the devbench plugin to enable",
			}
		};
	}

	// Lifecycle
	void Load() override;
	void Reset() override;

	// Settings persistence — no configurable settings remain; the overrides are
	// kept as no-ops so the feature still participates in the settings registry.
	void DrawSettings() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

	RemoteControl() = default;
	~RemoteControl() = default;

	RemoteControl(const RemoteControl&) = delete;
	RemoteControl& operator=(const RemoteControl&) = delete;
	RemoteControl(RemoteControl&&) = delete;
	RemoteControl& operator=(RemoteControl&&) = delete;
};
