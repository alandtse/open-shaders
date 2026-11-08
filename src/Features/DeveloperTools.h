#pragma once

#include "Feature.h"
#include "Utils/BootSnapshot.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief Owns the lifecycle and settings for RenderDoc, Nsight Perf and the devbench bridge.
 *
 * The tools themselves live in the RenderDoc and RemoteControl components (globals::features::renderDoc /
 * remoteControl); this feature owns their lifecycle, settings and UI.
 */
struct DeveloperTools : Feature
{
	static DeveloperTools* GetSingleton();

	/** @brief Nexus Mods page of the devbench SKSE plugin. */
	static constexpr const char* kDevBenchNexusUrl = "https://www.nexusmods.com/skyrimspecialedition/mods/181326";

	std::string GetName() override { return "Developer Tools"; }
	std::string GetDisplayName() override { return T("feature.developer_tools.name", "Developer Tools"); }
	std::string GetShortName() override { return "DeveloperTools"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool IsCore() const override { return true; }
	bool IsInMenu() const override { return true; }
	bool SupportsVR() override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.developer_tools.description", "Graphics debugging and profiling tools for {brand} development."),
			{ T("feature.developer_tools.key_feature_1", "RenderDoc frame capture with automatic capture comments"),
				T("feature.developer_tools.key_feature_2", "Nsight Perf GPU counters per render pass (NVIDIA)"),
				T("feature.developer_tools.key_feature_3", "devbench bridge for driving {brand} over MCP and REST") } };
	}

	void Load() override;
	void PostPostLoad() override;
	void SetupResources() override;
	void MigrateLegacySettings(json& config) const override;
	std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override;
	const void* GetBootValue(std::string_view key) const override;
	const void* GetSettingsBlob() const override;
	size_t GetSettingsBlobSize() const override;

	void DrawSettings() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

private:
	enum class Tool : int
	{
		RenderDoc,
		NsightPerf,
		DevBench,
		Count
	};

	/** @brief Opens the devbench Nexus Mods page in the default browser. */
	static void OpenDevBenchNexusPage();

	// UI state only: the tool whose card is selected and whose panel shows below the cards.
	Tool selectedTool = Tool::RenderDoc;
	// Whether the first draw has picked the tool in use.
	bool selectionInitialized = false;
};
