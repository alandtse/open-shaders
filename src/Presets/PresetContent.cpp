#include "PresetContent.h"

#include "Feature.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "UnifiedPresetCatalog.h"
#include "Utils/FileSystem.h"

nlohmann::json PresetContent::EditableValue(const File& file, const std::string& path)
{
	const nlohmann::json::json_pointer pointer(path);
	if (!file.document || !file.document->contains(pointer))
		return nullptr;
	const auto& value = file.document->at(pointer);
	const auto featureName = IsBaselineFile(file.relative) ? Util::PathToUtf8(file.relative.stem()) :
	                         IsSceneFile(file.relative)    ? SceneFeatureName(file.relative, *file.document) :
	                                                         std::string{};
	auto* feature = Feature::FindRegisteredFeatureByShortName(featureName);
	const auto* defaults = feature && globals::state ? globals::state->GetFeatureDefaults(feature->GetName()) : nullptr;
	if (defaults && defaults->contains(pointer)) {
		nlohmann::json normalized{ { "value", defaults->at(pointer) } };
		const auto result = ReplaceSetting(normalized, "/value", defaults->at(pointer), value);
		if (result == SettingEditResult::Changed || result == SettingEditResult::Unchanged)
			return normalized.at("value");
	}
	return value;
}

std::vector<PresetContent::File> PresetContent::List(const std::string& packId)
{
	const auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto* pack = catalog.FindPack(packId);
	if (!pack)
		return {};
	std::vector<File> files;
	if (pack->source == UnifiedPresetCatalog::SourceKind::LegacyBaseline) {
		for (const auto& source : SettingsOverrideManager::GetSingleton()->GetBaselinePacks())
			if (source.id == packId)
				for (const auto& [feature, document] : source.documents)
					files.push_back({ std::filesystem::path("Baseline") / (feature + ".json"), source.documentSources.at(feature), std::optional<nlohmann::json>{ document } });
		return files;
	}
	if (pack->source == UnifiedPresetCatalog::SourceKind::LegacyScene) {
		for (const auto& path : pack->legacySceneFiles)
			files.push_back({ path.lexically_relative(pack->rootPath), path, Util::FileHelpers::ReadJsonObject(path, "legacy scene", PresetState::kMaxDocumentBytes) });
		return files;
	}
	std::set<std::filesystem::path> seen;
	for (const auto& root : catalog.GetPackRoots(packId)) {
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
			!error && it != end; it.increment(error)) {
			if (!it->is_regular_file(error) || error)
				continue;
			const auto relative = it->path().lexically_relative(root);
			if (relative.empty() || relative.extension() != ".json")
				continue;
			const auto first = relative.begin()->string();
			if (!IsSceneFile(relative) && first != "Baseline" && first != "Forms")
				continue;
			if (seen.insert(relative).second)
				files.push_back({ relative, it->path(), Util::FileHelpers::ReadJsonObject(it->path(), "preset contents", PresetState::kMaxDocumentBytes) });
		}
	}
	std::ranges::sort(files, {}, &File::relative);
	return files;
}
