#include "FeatureOverwrites.h"

#include "BaselineManager.h"
#include "CSEditor/SceneManager/FeatureOverwritesPanel.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SceneSettingsManager.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsCatalog.h"

std::pair<std::string, std::vector<std::string>> FeatureOverwrites::GetFeatureSummary()
{
	return { T("feature.feature_overwrites.description", "Manage, edit and export feature overrides and Base Settings presets."), {} };
}

void FeatureOverwrites::DrawSettings()
{
	FeatureOverwritesPanel::Draw();
}

void FeatureOverwrites::RegisterUxActions()
{
	FEATURE_RENDER_QUERY("files", "List discovered feature override and enabled Baseline files with kind=override|baseline, filePath, feature, modName, packId, enabled and applicable. Legacy enabled comes from file metadata; applicable means the target is loaded or Global. Baseline files belong to their pack. Args: none.",
		([](const Feature*, const json&) {
			auto* manager = SettingsOverrideManager::GetSingleton();
			json result = json::array();
			for (const auto& info : manager->GetOverrides())
				result.push_back({ { "filePath", info.filePath }, { "feature", info.featureName }, { "modName", info.modName },
					{ "kind", "override" }, { "packId", "" }, { "enabled", info.enabled }, { "applicable", manager->IsApplicable(info) } });
			const auto* baselines = BaselineManager::GetSingleton();
			for (const auto& info : baselines->GetFiles())
				result.push_back({ { "filePath", info.filePath }, { "feature", info.featureName }, { "modName", info.packId },
					{ "kind", "baseline" }, { "packId", info.packId }, { "enabled", info.enabled }, { "applicable", baselines->IsApplicable(info) } });
			return result;
		}));
	FEATURE_RENDER_COMMAND("deleteFile", "Delete one discovered, applicable overwrite file and reapply affected settings, preserving saved personal settings, unsaved normal feature edits and Scene Manager drafts. Args: filePath from files, confirm=true. Removes companion user entries no remaining overwrite owns, deleting empty companion files. Permanent disk deletion; restore installed files by reinstalling their providing mod. Boot preferences update for the next restart.",
		([](Feature*, const json& args) {
			if (!args.value("confirm", false) || !SettingsOverrideManager::GetSingleton()->DeleteFile(args.at("filePath").get<std::string>()))
				throw std::runtime_error("Deletion requires a discovered file and confirm=true");
		}));
	FEATURE_RENDER_QUERY("exportSettings", "List exportable settings for one loaded feature. Args: feature=shortName. Returns path (JSON pointer) and display label for each setting, including catalogue-independent fallback entries. Arrays are one setting.",
		([](const Feature*, const json& args) {
			const auto name = args.at("feature").get<std::string>();
			auto* feature = Feature::FindFeatureByShortName(name);
			if (!feature || !feature->UsesMainSettings())
				throw std::runtime_error("Unknown or unavailable feature");
			json settings;
			globals::state->SaveToJson(settings);
			json result = json::array();
			for (const auto& setting : Util::Settings::GetExportSettings(name, settings.at(feature->GetName())))
				result.push_back({ { "path", setting.path }, { "label", setting.label } });
			return result;
		}));
	FEATURE_RENDER_COMMAND("exportSettings", "Export selected settings from one loaded feature, including unsaved normal feature edits but excluding applied Scene Manager values and toolbar drafts. Args: kind=override(default)|baseline, modName=nonempty file prefix or pack name, feature=shortName, settings=[JSON pointer paths from exportSettings query], optional removeSettings=[JSON pointer paths]. Writes Overrides/ModName_Feature.json or Presets/Pack/Baseline/Feature.json while preserving other keys and metadata. Removed override settings release orphaned companion entries; saved personal settings remain intact. Rejects empty/unknown paths, partial array components and externally changed discovered overrides until settings are reloaded. Baseline exports refresh and reapply the pack when enabled. Does not save the main config.",
		([](Feature*, const json& args) {
			json settings;
			globals::state->SaveToJson(settings);
			const auto paths = args.at("settings").get<std::vector<std::string>>();
			const auto removePaths = args.value("removeSettings", std::vector<std::string>{});
			const auto name = args.at("modName").get<std::string>();
			const auto featureName = args.at("feature").get<std::string>();
			const auto kind = args.value("kind", std::string("override"));
			if (kind == "baseline") {
				auto* feature = Feature::FindFeatureByShortName(featureName);
				if (!feature || !BaselineManager::GetSingleton()->ExportSettings(name, featureName, paths,
									settings.at(feature->GetName()), removePaths))
					throw std::runtime_error("Baseline setting export failed");
				SceneSettingsManager::GetSingleton()->ReleaseSketches(featureName);
				auto& catalog = UnifiedPresetCatalog::GetSingleton();
				catalog.Discover();
				catalog.ReapplyIfInUse(Util::FileHelpers::SanitizeFileName(name));
			} else if (kind != "override") {
				throw std::invalid_argument("Unknown export destination kind");
			} else if (!SettingsOverrideManager::GetSingleton()->ExportSettings(name,
						   featureName, paths, settings, removePaths))
				throw std::runtime_error("Feature setting export failed");
		}));
}
