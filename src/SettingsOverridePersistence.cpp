#include "SettingsOverrideManager.h"

#include "Feature.h"
#include "I18n/I18n.h"
#include "Presets/LegacyPreset.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsPatch.h"

namespace
{
	std::vector<LegacyPreset::FeatureNames> GetFeatureNames()
	{
		std::vector<LegacyPreset::FeatureNames> names;
		for (auto* feature : Feature::GetFeatureList())
			if (feature->UsesMainSettings())
				names.emplace_back(feature->GetShortName(), feature->GetName());
		return names;
	}
}

const std::vector<SettingsOverrideManager::LegacyPack>& SettingsOverrideManager::GetBaselinePacks() const
{
	if (translatedPacks)
		return *translatedPacks;
	std::map<std::string, LegacyPack> packs;
	const auto names = GetFeatureNames();
	for (bool global : { true, false }) {
		for (const auto& info : overrides) {
			if (!info.enabled || info.isGlobal != global)
				continue;
			auto documents = LegacyPreset::Translate(info.overrideData, info.featureName, names);
			for (auto& [name, document] : documents) {
				auto* feature = Feature::FindRegisteredFeatureByShortName(name);
				const auto* defaults = globals::state && feature ? globals::state->GetFeatureDefaults(feature->GetName()) : nullptr;
				if (!defaults || defaults->empty())
					continue;
				auto normalized = LegacyPreset::NormalizePayload(document, *defaults);
				for (const auto& key : normalized.retiredKeys)
					logger::warn("[Presets] Legacy file '{}' has retired setting '{}'; importing its recognized settings without changing the source file", info.filePath, key);
				document = std::move(normalized.values);
			}
			std::erase_if(documents, [](const auto& item) { return item.second.empty(); });
			const auto boot = info.isGlobal ? LegacyPreset::TranslateBoot(info.overrideData, names) : std::unordered_map<std::string, bool>{};
			if (documents.empty() && boot.empty())
				continue;
			const auto id = LegacyPreset::BuildPackId(Util::FileHelpers::SanitizeFileName(info.modName), global);
			if (!PresetState::IsPackId(id))
				continue;
			auto& pack = packs[id];
			pack.id = id;
			pack.name = global ? std::vformat(T("menu.presets.legacy_global_name", "{} (Global)"), std::make_format_args(info.modName)) : info.modName;
			pack.version = info.version;
			pack.description = info.description;
			pack.sourcePath = Util::PathHelpers::FromUtf8(info.filePath);
			for (const auto& [name, disabled] : boot)
				pack.disableAtBoot[name] = disabled;
			for (const auto& [name, values] : documents) {
				pack.documentSources[name] = Util::PathHelpers::FromUtf8(info.filePath);
				auto& payload = pack.documents[name];
				if (!payload.is_object())
					payload = json::object();
				payload.update(values, true);
			}
		}
	}
	std::vector<LegacyPack> result;
	for (auto& [_, pack] : packs)
		result.push_back(std::move(pack));
	translatedPacks = std::move(result);
	return *translatedPacks;
}

json SettingsOverrideManager::GetPendingEdits(const json& settings) const
{
	return Util::Settings::BuildUserOverride(settings, appliedSettings);
}
