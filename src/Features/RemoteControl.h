#pragma once

#include "Feature.h"

#include <string>

// Status panel for the devbench bridge. The plugin's tools are registered into the external
// devbench host via DevBenchBridge (see src/Features/RemoteControl/DevBenchBridge.cpp); this feature surfaces, in the
// in-game menu, whether devbench is present, what was registered, and which port the host bound.
class RemoteControl : public Feature
{
public:
	static RemoteControl* GetSingleton();

	// Feature overrides — see Feature.h for contracts. Only members that differ from the base
	// defaults are overridden (the feature has no settings, shaders, or per-frame state).
	std::string GetName() override { return "Remote Control"; }
	std::string GetShortName() override { return "RemoteControl"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool SupportsVR() override { return true; }

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

	// The bridge installs at DataLoaded (not Load): Load runs during SKSEPluginLoad, before
	// devbench's kPostLoad init, so its cross-plugin interface isn't ready yet.
	void DataLoaded() override;
	void DrawSettings() override;

	RemoteControl() = default;
	~RemoteControl() = default;

	RemoteControl(const RemoteControl&) = delete;
	RemoteControl& operator=(const RemoteControl&) = delete;
	RemoteControl(RemoteControl&&) = delete;
	RemoteControl& operator=(RemoteControl&&) = delete;
};
