#include "SceneManager.h"

#include "BaselineManager.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SceneSettingsContextRules.h"
#include "SceneSettingsLocationTargets.h"
#include "SceneSettingsUI.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Utils/FileSystem.h"
#include "Utils/Form.h"
#include "Utils/Game.h"
#include "Utils/SettingsCatalog.h"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <string_view>

namespace
{
	using Manager = SceneSettingsManager;
	using Context = Manager::SceneContextId;
	using ContextType = Manager::SceneContextType;
	using Source = Manager::EntrySource;
	using Period = Manager::TimeOfDayPeriod;

	bool IsSceneReady()
	{
		const auto* player = globals::game::player;
		return player && player->parentCell && globals::state &&
		       !globals::state->IsMainOrLoadingMenuOpen(globals::game::ui);
	}

	Manager& RequireReady()
	{
		if (!IsSceneReady())
			throw std::invalid_argument("Load a player cell and wait for the loading screen to close first");
		return *Manager::GetSingleton();
	}

	Feature* RequireFeature(const json& args)
	{
		auto* feature = Feature::FindFeatureByShortName(args.at("feature").get<std::string>());
		if (!feature || !feature->loaded)
			throw std::invalid_argument("feature must name a loaded feature");
		return feature;
	}

	Context ParseContext(const json& args)
	{
		Context context;
		const auto type = args.at("type").get<std::string>();
		if (type == "interior")
			context.type = ContextType::Interior;
		else if (type == "timeOfDay")
			context.type = ContextType::TimeOfDay;
		else if (type == "weather") {
			context.type = ContextType::Weather;
			context.weatherId = Util::SpidToFormId(args.at("formKey").get<std::string>());
			if (!RE::TESForm::LookupByID<RE::TESWeather>(context.weatherId))
				throw std::invalid_argument("formKey must resolve to a weather");
		} else if (type == "location") {
			context.type = ContextType::Location;
			const auto locationType = args.at("locationType").get<std::string>();
			const auto found = std::ranges::find_if(Manager::kLocationTargetTypes, [&](const auto candidate) {
				return locationType == Manager::GetLocationTargetTypeName(candidate);
			});
			if (found == Manager::kLocationTargetTypes.end())
				throw std::invalid_argument("locationType must be Worldspace, Region, LocationType, Location or Cell");
			context.locationType = *found;
			const auto formKey = args.at("formKey").get<std::string>();
			const auto targets = SceneSettingsLocationTargets::ResolveLocationTargetChain(context.locationType, formKey);
			const auto id = Util::SpidToFormId(formKey);
			const auto target = std::ranges::find_if(targets, [&](const auto& candidate) {
				return candidate.type == context.locationType && Util::SpidToFormId(candidate.formKey) == id;
			});
			if (!id || target == targets.end())
				throw std::invalid_argument("formKey and locationType must identify an available location target");
			context.locationFormKey = target->formKey;
		} else {
			throw std::invalid_argument("type must be interior, timeOfDay, weather or location");
		}
		const auto period = args.value("period", std::string("Normal"));
		context.period = Manager::GetPeriodFromName(period);
		if ((period != "Normal" && context.period == Period::Count) ||
			(context.type == ContextType::TimeOfDay && context.period == Period::Count) ||
			(context.type == ContextType::Interior && context.period != Period::Count))
			throw std::invalid_argument("period must match the scene type: Normal or Dawn, Sunrise, Day, Sunset, Dusk, Night");
		return context;
	}

	const Manager::SettingEntry& RequireUserEntry(const Manager& manager, const Context& context, size_t index)
	{
		const auto entries = manager.GetContextEntries(context);
		if (index >= entries.size() || entries[index].source != Source::User ||
			!SceneSettingsContextRules::EntryBelongsToContext(entries[index], context))
			throw std::invalid_argument("index must identify a User entry in the requested period; refresh sceneSets first");
		if (!manager.IsContextEditPreviewActive() && manager.IsContextEditTarget(context, entries[index].featureShortName))
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
		return entries[index];
	}

	json CurrentLocations(const Feature*, const json&)
	{
		json targets = json::array();
		for (const auto& target : Manager::GetSingleton()->GetCurrentLocationTargets())
			targets.push_back({ { "type", Manager::GetLocationTargetTypeName(target.type) },
				{ "formKey", target.formKey }, { "name", target.name }, { "editorId", target.editorId }, { "formId", target.formId } });
		return targets;
	}

	json SceneSets(const Feature*, const json& args)
	{
		const auto context = ParseContext(args);
		const auto& manager = *Manager::GetSingleton();
		json result{ { "sceneReady", IsSceneReady() }, { "timeOfDayEnabled", manager.IsSceneTimeOfDayEnabled(context) },
			{ "entryRevision", manager.GetEntryPresentationRevision() }, { "entries", json::array() } };
		const auto entries = manager.GetContextEntries(context);
		for (size_t index = 0; index < entries.size(); ++index) {
			const auto& entry = entries[index];
			result["entries"].push_back({ { "index", index }, { "feature", entry.featureShortName },
				{ "path", entry.settingPath }, { "setting", entry.settingKey },
				{ "period", entry.period == Period::Count ? "Normal" : Manager::GetPeriodName(entry.period) },
				{ "source", entry.source == Source::User ? "User" : "Overwrite" }, { "sourceFilename", entry.sourceFilename },
				{ "value", entry.value }, { "originalValue", entry.originalValue }, { "paused", entry.paused },
				{ "deleted", entry.deleted }, { "selectedPeriod", SceneSettingsContextRules::EntryBelongsToContext(entry, context) },
				{ "transitionSeconds", entry.transitionSeconds ? json(*entry.transitionSeconds) : json(nullptr) } });
		}
		return result;
	}

	json FeaturePauseState(const Feature*, const json& args)
	{
		const auto context = ParseContext(args);
		const auto feature = args.at("feature").get<std::string>();
		size_t count = 0, paused = 0, userCount = 0, userPaused = 0, activeOverwrites = 0;
		for (const auto& entry : Manager::GetSingleton()->GetContextEntries(context)) {
			if (entry.featureShortName != feature || !SceneSettingsContextRules::EntryBelongsToContext(entry, context))
				continue;
			++count;
			paused += entry.paused;
			userCount += entry.source == Source::User;
			userPaused += entry.source == Source::User && entry.paused;
			activeOverwrites += entry.source == Source::Overwrite && !entry.paused && !entry.deleted;
		}
		return { { "featurePaused", Manager::GetSingleton()->IsFeaturePaused(feature) }, { "count", count }, { "paused", paused }, { "userCount", userCount }, { "userPaused", userPaused },
			{ "activeOverwrites", activeOverwrites }, { "sceneReady", IsSceneReady() },
			{ "toolbarOpen", SceneSettingsUI::IsFeaturePageEditing(Feature::FindFeatureByShortName(feature)) },
			{ "previewEditing", Manager::GetSingleton()->IsContextEditing(feature) },
			{ "previewActive", Manager::GetSingleton()->IsContextEditing(feature) && Manager::GetSingleton()->IsContextEditPreviewActive() },
			{ "previewPendingEdits", Manager::GetSingleton()->IsContextEditing(feature) && Manager::GetSingleton()->HasPendingContextEdit() } };
	}

	json BlendState(const Feature*, const json&)
	{
		const auto factors = Manager::GetSingleton()->GetTimeOfDayFactors();
		json result{ { "sceneReady", IsSceneReady() }, { "hour", Manager::GetCurrentGameHour() }, { "periodFactors", json::object() } };
		for (size_t index = 0; index < factors.size(); ++index)
			result["periodFactors"][Manager::GetPeriodName(static_cast<Period>(index))] = factors[index];
		if (const auto* sky = globals::game::sky) {
			result["currentWeather"] = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
			result["previousWeather"] = sky->lastWeather ? sky->lastWeather->GetFormID() : 0;
			result["weatherBlend"] = sky->currentWeatherPct;
		}
		return result;
	}

	json EnvironmentState(const Feature*, const json&)
	{
		const auto* weather = Util::EnvironmentControls::GetLockedWeather();
		const auto* calendar = globals::game::calendar;
		return { { "playing", Util::EnvironmentControls::IsPreviewActive() },
			{ "weather", weather ? weather->GetFormID() : 0 }, { "timePaused", Util::EnvironmentControls::IsTimePaused() },
			{ "hour", calendar && calendar->gameHour ? json(calendar->gameHour->value) : json(nullptr) },
			{ "timeScale", calendar && calendar->timeScale ? json(calendar->timeScale->value) : json(nullptr) } };
	}

	json CopyCandidates(const Feature*, const json& args)
	{
		const auto source = ParseContext(args.at("source"));
		const auto destination = ParseContext(args.at("destination"));
		json candidates = json::array();
		for (const auto& candidate : Manager::GetSingleton()->GetCopyCandidates(source, destination))
			candidates.push_back({ { "feature", candidate.setting.featureShortName }, { "path", candidate.setting.settingPath },
				{ "setting", candidate.setting.settingKey }, { "value", candidate.value },
				{ "destinationValue", candidate.destinationValue.value_or(json(nullptr)) },
				{ "compatible", candidate.compatible }, { "conflicts", candidate.conflicts },
				{ "rejection", magic_enum::enum_name(candidate.rejection) } });
		return candidates;
	}

	void OpenFeatureToolbar(Feature*, const json& args)
	{
		RequireReady();
		auto* feature = RequireFeature(args);
		if (!SceneSettingsUI::CanEditFeaturePage(feature))
			throw std::invalid_argument("The feature has no editable scene context at the current place");
		if (!SceneSettingsUI::BeginFeaturePageEditing(feature))
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
	}

	void SetSceneEntriesPaused(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		const auto feature = RequireFeature(args)->GetShortName();
		const bool paused = args.at("paused").get<bool>();
		if (!manager.IsContextEditPreviewActive() && manager.IsContextEditTarget(context, feature))
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
		const auto entries = manager.GetContextEntries(context);
		std::vector<size_t> indices;
		for (size_t index = 0; index < entries.size(); ++index) {
			const auto& entry = entries[index];
			if (entry.source == Source::User && entry.featureShortName == feature && entry.paused != paused &&
				SceneSettingsContextRules::EntryBelongsToContext(entry, context))
				indices.push_back(index);
		}
		for (const auto index : indices)
			manager.TogglePauseContextEntry(context, index);
	}

	void AddSetting(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		const auto feature = RequireFeature(args)->GetShortName();
		if (!manager.IsContextEditPreviewActive() && manager.IsContextEditTarget(context, feature))
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
		if (!manager.AddContextSetting(context, feature, args.value("path", std::vector<std::string>{}), args.at("setting").get<std::string>()))
			throw std::invalid_argument("Setting is unavailable, incompatible or already authored in this context");
	}

	void UpdateEntries(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		std::vector<Manager::EntryValueUpdate> updates;
		std::set<size_t> indices;
		for (const auto& update : args.at("updates")) {
			const auto index = update.at("index").get<size_t>();
			const auto& entry = RequireUserEntry(manager, context, index);
			if (entry.deleted || !indices.insert(index).second)
				throw std::invalid_argument("updates must use distinct User entries that are not tombstones");
			updates.push_back({ index, update.at("value") });
		}
		if (updates.empty())
			throw std::invalid_argument("updates must not be empty");
		manager.UpdateContextEntryValues(context, updates);
		for (const auto& update : updates)
			if (manager.GetContextEntries(context)[update.index].value != update.value)
				throw std::invalid_argument("The batch was rejected by scene setting validation; read sceneSets to inspect values");
	}

	void ResetEntry(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		const auto index = args.at("index").get<size_t>();
		if (RequireUserEntry(manager, context, index).deleted)
			throw std::invalid_argument("Remove the tombstone to restore this context's setting");
		manager.RevertContextEntryToDefault(context, index);
	}

	void RemoveEntries(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		auto indices = args.at("indices").get<std::vector<size_t>>();
		std::ranges::sort(indices, std::greater<>{});
		if (std::adjacent_find(indices.begin(), indices.end()) != indices.end())
			throw std::invalid_argument("indices must be distinct");
		for (const auto index : indices)
			RequireUserEntry(manager, context, index);
		for (const auto index : indices)
			manager.RemoveContextSetting(context, index);
	}

	void SuppressSetting(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		const auto feature = args.at("feature").get<std::string>();
		if (!manager.IsContextEditPreviewActive() && manager.IsContextEditTarget(context, feature))
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
		if (!manager.TombstoneContextSetting(context, feature,
				args.value("path", std::vector<std::string>{}), args.at("setting").get<std::string>()))
			throw std::invalid_argument("No compatible scene value can be suppressed at this address");
	}

	void CopyContextSettings(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto source = ParseContext(args.at("source"));
		const auto destination = ParseContext(args.at("destination"));
		const auto policy = args.value("conflicts", std::string("skip"));
		const auto conflictPolicy = policy == "skip"      ? Manager::CopyConflictPolicy::SkipExisting :
		                            policy == "overwrite" ? Manager::CopyConflictPolicy::OverwriteExisting :
		                            policy == "cancel"    ? Manager::CopyConflictPolicy::Cancel :
		                                                    throw std::invalid_argument("conflicts must be skip, overwrite or cancel");
		const auto result = manager.CopySettings(source, destination, conflictPolicy);
		if (result.blockedByPendingEdit)
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
		if (result.cancelled)
			throw std::invalid_argument("Copy cancelled because destination settings conflict");
	}

	void SetLocationTransition(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		if (context.type != ContextType::Location)
			throw std::invalid_argument("type must be location");
		const auto indices = args.at("indices").get<std::vector<size_t>>();
		for (const auto index : indices)
			RequireUserEntry(manager, context, index);
		const std::optional<float> seconds = args.at("seconds").is_null() ? std::nullopt : std::optional{ args.at("seconds").get<float>() };
		manager.SetLocationEntryTransitionSeconds(context.locationType, context.locationFormKey, indices, seconds);
	}

	void SetSceneMode(Feature*, const json& args)
	{
		auto& manager = RequireReady();
		const auto context = ParseContext(args);
		if (context.type != ContextType::Weather && context.type != ContextType::Location)
			throw std::invalid_argument("type must be weather or location");
		if (manager.IsContextEditModeTarget(context))
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
		manager.SetSceneTimeOfDayEnabled(context, args.at("timeOfDayEnabled").get<bool>());
	}

	void ReloadSettings(Feature*, const json&)
	{
		auto& manager = RequireReady();
		if (manager.HasPendingContextEdit())
			throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
		if (!manager.ReloadSceneSettings())
			throw std::runtime_error("Scene settings could not be reloaded");
	}

	Manager::EntryLayerScope ParseEntryLayerScope(const json& args)
	{
		Manager::EntryLayerScope scope;
		const auto type = args.value("type", std::string("all"));
		if (type != "all") {
			const std::array<std::string_view, 4> names{ "interior", "timeOfDay", "weather", "location" };
			const std::array types{ ContextType::Interior, ContextType::TimeOfDay, ContextType::Weather, ContextType::Location };
			const auto selected = std::ranges::find(names, type);
			if (selected == names.end())
				throw std::invalid_argument("type must be all, interior, timeOfDay, weather or location");
			scope.type = types[selected - names.begin()];
		}
		if (args.contains("target")) {
			scope.target = ParseContext(args.at("target"));
			if (scope.type && *scope.type != scope.target->type)
				throw std::invalid_argument("target must belong to the selected scene type");
			scope.type = scope.target->type;
		}
		return scope;
	}

	Source ParseEntryLayerSource(const json& args)
	{
		const auto source = args.at("source").get<std::string>();
		if (source == "user")
			return Source::User;
		if (source == "overwrite")
			return Source::Overwrite;
		throw std::invalid_argument("source must be user or overwrite");
	}
}

void SceneManager::RegisterUxActions()
{
	FEATURE_RENDER_QUERY("exportBaseSettings", "List exportable Base Settings paths for one loaded feature. Args: feature=shortName. Returns path and label; arrays are exported whole. No files are changed.",
		([](const Feature*, const json& args) {
			auto* feature = RequireFeature(args);
			if (!feature->UsesMainSettings())
				throw std::invalid_argument("The feature has no Base Settings");
			json values;
			globals::state->SaveToJson(values);
			json result = json::array();
			for (const auto& setting : Util::Settings::GetExportSettings(feature->GetShortName(), values.at(feature->GetName())))
				result.push_back({ { "path", setting.path }, { "label", setting.label } });
			return result;
		}));
	FEATURE_RENDER_COMMAND("exportBaseSettings", "Export selected settings into an upstream-compatible Base Settings preset, excluding applied scene values. Args: packName, feature=shortName, settings=[JSON pointer paths from exportBaseSettings query], optional removeSettings=[paths]. Preserves other file keys and pack metadata. Legacy imports are not export destinations; use a new preset name. Original legacy files are never written by export. New standalone Base Settings packs enable automatically. Does not save the main settings file.",
		([](Feature*, const json& args) {
			auto* feature = RequireFeature(args);
			json values;
			globals::state->SaveToJson(values);
			const auto name = args.at("packName").get<std::string>();
			if (!feature->UsesMainSettings() || !BaselineManager::GetSingleton()->ExportSettings(name, feature->GetShortName(),
													args.at("settings").get<std::vector<std::string>>(), values.at(feature->GetName()), args.value("removeSettings", std::vector<std::string>{})))
				throw std::runtime_error("Base Settings export failed");
			auto& catalog = UnifiedPresetCatalog::GetSingleton();
			if (!catalog.Refresh())
				throw std::runtime_error("Could not apply exported Base Settings");
		}));
	FEATURE_RENDER_QUERY("sceneLayerSummary",
		"Read the Manage Settings source layer across all saved periods. Args: source=user|overwrite, type=all|interior|timeOfDay|weather|location (all default), optional target object using sceneSets context args to narrow to one target. The target period is ignored. Returns total and paused. Requires a loaded player cell; no settings are changed.",
		([](const Feature*, const json& args) -> json {
			const auto summary = RequireReady().GetEntryLayerSummary(ParseEntryLayerSource(args), ParseEntryLayerScope(args));
			return { { "total", summary.total }, { "paused", summary.paused } };
		}));
	FEATURE_RENDER_COMMAND("setSceneLayerPaused",
		"Pause or resume the Manage Settings source layer. Args: source=user|overwrite, paused=boolean, type=all|interior|timeOfDay|weather|location (all default), optional target object using sceneSets context args. Acts across all saved periods; a target period is ignored. User pause state persists; overwrite pauses are session-only and reset on reload. Refuses any unsaved retained scene draft and invalidates a clean retained draft. Verify sceneLayerSummary and live values afterward.",
		([](Feature*, const json& args) {
			auto& manager = RequireReady();
			if (manager.HasPendingContextEdit())
				throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
			manager.SetEntryLayerPaused(ParseEntryLayerSource(args), args.at("paused").get<bool>(), ParseEntryLayerScope(args));
		}));
	FEATURE_RENDER_QUERY("presets",
		"List unified preset packs and current selection. Args: none. Returns activePack, enabled baselinePacks in precedence order, baselineOrder including disabled packs, and packs with id/name/version, valid/error, scene/form/Baseline payload flags, enabled scene state, automatic startup activation and conflict blockers. This reads the current catalog; refresh through managePreset action=refresh.",
		([](const Feature*, const json&) -> json {
			auto& catalog = UnifiedPresetCatalog::GetSingleton();
			json packs = json::array();
			for (const auto& pack : catalog.GetPacks())
				packs.push_back({ { "id", pack.id }, { "name", pack.name }, { "version", pack.version },
					{ "valid", pack.valid }, { "error", pack.invalidReason }, { "hasScenes", pack.hasCSPresets },
					{ "hasForms", pack.hasFormEdits }, { "hasBaseline", pack.hasBaseline },
					{ "baselineEnabled", catalog.IsBaselineEnabled(pack.id) }, { "sceneEnabled", catalog.IsScenePackEnabled(pack.id) }, { "conflicts", catalog.GetConflictingPacks(pack.id) } });
			return { { "activePack", catalog.GetActivePackId() }, { "baselinePacks", catalog.GetBaselinePackIds() }, { "baselineOrder", catalog.GetBaselinePackOrder() }, { "packs", packs }, { "automatic", catalog.GetAutomaticPresets() } };
		}));
	FEATURE_RENDER_COMMAND("managePreset",
		"Use the Presets page production paths. Args: action=refresh|apply|disable|delete|removeBaseline|raiseBaseline|lowerBaseline|automatic, id for pack actions; disable with id disables all of that pack's layers, without id disables the active scene pack. delete disables a library pack and moves its folder into the hidden deleted-presets library; the legacy game/Data install cannot be deleted. saveCurrent=boolean for apply (true default), replaceConflicts=true explicitly disables conflicting scene presets. raiseBaseline/lowerBaseline also retain priority for disabled packs. automatic requires enabled=boolean and controls startup discovery for both scene and Base Settings presets; scenes reject conflicts, Base Settings layer together. Requires a loaded player cell except for automatic. Selection changes refuse an unsaved retained scene draft. Verify with presets and live feature queries.",
		[](Feature*, const json& args) {
			auto& catalog = UnifiedPresetCatalog::GetSingleton();
			const auto action = args.at("action").get<std::string>();
			if (action != "automatic")
				RequireReady();
			if (action != "refresh" && action != "automatic" && Manager::GetSingleton()->HasPendingContextEdit())
				throw std::invalid_argument(SceneSettingsUI::PendingContextEditMessage());
			bool applied = true;
			if (action == "automatic")
				applied = catalog.SetAutomaticPresets(args.at("enabled").get<bool>());
			else if (action == "refresh") {
				applied = catalog.Refresh();
			} else if (action == "disable") {
				if (args.contains("id"))
					applied = catalog.DisablePack(args.at("id").get<std::string>());
				else {
					catalog.DisableActivePack();
					applied = catalog.GetActivePackId().empty();
				}
			} else if (action == "apply")
				applied = catalog.ApplyPack(args.at("id").get<std::string>(), args.value("saveCurrent", true), args.value("replaceConflicts", false));
			else if (action == "delete")
				applied = catalog.DeletePack(args.at("id").get<std::string>());
			else if (action == "removeBaseline")
				applied = catalog.RemoveBaseline(args.at("id").get<std::string>());
			else if (action == "raiseBaseline" || action == "lowerBaseline")
				applied = catalog.MoveBaseline(args.at("id").get<std::string>(), action == "raiseBaseline");
			else
				throw std::invalid_argument("Unknown preset action");
			if (!applied)
				throw std::runtime_error("Preset operation was refused or failed; inspect presets and the game log");
		});
	FEATURE_QUERY("currentLocations",
		"Read the current location targets in resolver order, from general to specific. Args: none. Returns type, formKey, name, editorId and formId. Includes the parent-location chain; only the innermost location supplies location-type keywords. Worldspace membership follows the exterior cell.",
		CurrentLocations);
	FEATURE_RENDER_QUERY("presetSceneSettings",
		"Read compatible scene settings from one installed preset, including inactive legacy imports. Args: id from presets query. Returns stable key, scene label, feature, setting and value. No files or selections are changed. Use keys with copyPresetSettings.",
		([](const Feature*, const json& args) -> json {
			auto& manager = RequireReady();
			json result = json::array();
			for (const auto& setting : manager.GetPresetSceneSettings(args.at("id").get<std::string>()))
				if (!setting.entry.deleted)
					result.push_back({ { "key", setting.key }, { "scene", manager.GetSceneContextDisplayName(setting.context) },
						{ "feature", setting.entry.featureShortName }, { "setting", setting.entry.displayName }, { "value", setting.entry.value } });
			return result;
		}));
	FEATURE_RENDER_COMMAND("copyPresetSettings",
		"Copy selected installed-preset scene values to matching User Settings, including inactive legacy presets. Args: id, settings=[stable keys from presetSceneSettings], overwrite=boolean (false default keeps existing User Settings). Original preset files are unchanged. Logical controls copy together; a conflict skips the whole control unless overwrite is true. Pending scene drafts block copying. Verify sceneSets afterward.",
		([](Feature*, const json& args) {
			auto& manager = RequireReady();
			const auto result = manager.CopyPresetSceneSettings(args.at("id").get<std::string>(), args.at("settings").get<std::vector<std::string>>(),
				args.value("overwrite", false) ? Manager::CopyConflictPolicy::OverwriteExisting : Manager::CopyConflictPolicy::SkipExisting);
			if (result.cancelled)
				throw std::runtime_error("Preset copy was refused or could not be saved");
		}));
	FEATURE_RENDER_COMMAND("deletePresetSettings",
		"Delete selected settings or one backing file through preset management. Args: id, file=relative payload path, settings=[JSON pointer paths], entireFile=boolean (false default). Legacy originals change only through this explicit destructive action. Pending scene drafts block deletion; failed reapplication restores the source. Refresh presets afterward.",
		([](Feature*, const json& args) {
			RequireReady();
			if (!UnifiedPresetCatalog::GetSingleton().DeletePackSettings(args.at("id").get<std::string>(), args.at("file").get<std::string>(),
					args.value("settings", std::vector<std::string>{}), args.value("entireFile", false)))
				throw std::runtime_error("Preset deletion was refused or failed");
		}));
	FEATURE_QUERY("sceneSets",
		"Read current scene entries and mode, including an active toolbar's unsaved preview. Args: type=interior|timeOfDay|weather|location; period=Normal|Dawn|Sunrise|Day|Sunset|Dusk|Night (Normal default, named period required for timeOfDay, only Normal for interior). Weather/location require formKey=SPID; location requires locationType=Worldspace|Region|LocationType|Location|Cell. Returns every period and ownership layer, with source, deleted tombstones, values and indices. selectedPeriod marks the requested set. Refresh indices after structural mutations, preview suspension or resume. timeOfDayEnabled selects the active weather/location set without deleting the other set.",
		SceneSets);
	FEATURE_QUERY("featureScenePauseState",
		"Read one feature's counts, session-only featurePaused, and retained toolbar state. Args: feature=shortName plus sceneSets context args. count/paused cover both sources; userCount/userPaused cover editable User entries; activeOverwrites counts unpaused, nondeleted mod entries. toolbarOpen reports the visible enabled toolbar; previewEditing identifies its retained draft, previewActive its applied preview, and previewPendingEdits changes awaiting explicit Save. No settings are changed.",
		FeaturePauseState);
	FEATURE_QUERY("blendState",
		"Read sceneReady, game hour, climate-derived periodFactors, and currentWeather/previousWeather FormIDs with weatherBlend. Args: none. Does not advance time or change weather.",
		BlendState);
	FEATURE_QUERY("environmentPreviewState",
		"Read the shared environment controls: playing, locked weather FormID or 0, timePaused, hour and timeScale. Args: none. playing describes shared environment preview ownership, not a retained scene draft.",
		EnvironmentState);
	FEATURE_QUERY("copySceneCandidates",
		"Preview a whole-context copy without changing settings. Args: source and destination objects using sceneSets context args. Returns setting identity, value, destinationValue, compatible, conflicts and rejection. Unpaused destination overwrites can reject candidates. Use before copySceneSettings.",
		CopyCandidates);
	FEATURE_RENDER_COMMAND("openFeatureSceneToolbar",
		"Open or resume a loaded feature's retained scene toolbar. Args: feature=shortName. Navigate to that feature's main menu page to see it. Chooses the current interior/time-of-day, weather or location context. Edits preview without persisting until saveFeatureSceneEdit. Closing suspends the preview and keeps the draft; replacing a dirty draft asks for discard confirmation. Does not start weather/time playback.",
		OpenFeatureToolbar);
	FEATURE_RENDER_COMMAND("openFeatureSceneEditor",
		"Alias for openFeatureSceneToolbar. Args: feature=shortName. Opens or resumes the retained draft on the feature's main menu page without saving it.",
		OpenFeatureToolbar);
	FEATURE_RENDER_COMMAND("saveFeatureSceneEdit",
		"Press the retained feature toolbar's Save. Args: feature=shortName. Requires its active preview. Atomically saves its User entries and mode; a failed write leaves the draft available for retry. Verify featureScenePauseState previewPendingEdits and sceneSets.",
		[](Feature*, const json& args) {
			auto& manager = RequireReady();
			if (!manager.IsContextEditing(args.at("feature").get<std::string>()) || !manager.IsContextEditPreviewActive())
				throw std::invalid_argument("Open the feature's retained scene toolbar first");
			if (!manager.SaveContextEdit())
				throw std::runtime_error(T("feature.scene_manager.edit.save_failed", "Could not save scene settings. Your unsaved changes are kept. Try Save again."));
		});
	FEATURE_RENDER_COMMAND("hideFeatureSceneToolbar",
		"Close the retained scene toolbar and stop its environment preview. Args: none. Suspends unsaved scene application without saving or discarding its entries; opening the same feature resumes them.",
		[](Feature*, const json&) { SceneSettingsUI::HideFeaturePageEditing(); });
	FEATURE_RENDER_COMMAND("setEnvironmentPreviewPlaying",
		"Press the retained feature toolbar's Play or Stop. Args: playing=boolean, feature=shortName required for Play. Locks selected weather/time and can travel to a location. Stop restores prior controls without rewinding the hour. Does not save scene entries. Verify environmentPreviewState.",
		[](Feature*, const json& args) {
			const bool playing = args.at("playing").get<bool>();
			if (playing && !SceneSettingsUI::IsFeaturePageEditing(RequireFeature(args)))
				throw std::invalid_argument("Open the feature's retained scene toolbar first");
			if (!SceneSettingsUI::SetFeaturePagePreviewPlaying(playing))
				throw std::invalid_argument("The selected scene cannot be previewed or the game is not ready");
		});
	FEATURE_COMMAND("setFeatureApplicationPaused",
		"Temporarily pause or resume all scene settings for one loaded feature, without changing saved entries. Pausing restores its editable base settings; resuming reapplies the current scene. Args: feature=shortName, paused=boolean. This state lasts for the current session. Verify featurePaused with featureScenePauseState.",
		([](Feature*, const json& args) {
			auto* feature = RequireFeature(args);
			SceneSettingsManager::GetSingleton()->SetFeaturePaused(feature->GetShortName(), args.at("paused").get<bool>());
		}));
	FEATURE_COMMAND("setFeatureScenePaused",
		"Persist paused=boolean for User entries of one feature in one scene context. Args: feature, paused, plus sceneSets context args. Author overwrite entries are left intact. Does not change scene mode, create entries or pause the whole feature. Verify with featureScenePauseState. Active retained-toolbar entries stay unsaved until saveFeatureSceneEdit; a suspended dirty draft refuses edits to its owned entries.",
		SetSceneEntriesPaused);
	FEATURE_COMMAND("addSceneSetting",
		"Add one User setting with the production add-setting path. Args: feature=shortName, setting=key, path=string array (empty default), plus sceneSets context args. Captures its lower-layer/base value and persists; incompatible settings and duplicate User entries are refused. Does not change weather/location mode. Read sceneSets before updating its value. Active retained-toolbar entries stay unsaved until saveFeatureSceneEdit; a suspended dirty draft refuses edits to its owned entries.",
		AddSetting);
	FEATURE_COMMAND("updateSceneEntries",
		"Validate and update a batch of User values, then persist and reapply. Args: sceneSets context args plus updates=[{index,value}]. Entries must belong to the requested period, have distinct indices and not be tombstones. Author overwrite values are never edited by this command. Refresh indices and verify with sceneSets. Active retained-toolbar entries stay unsaved until saveFeatureSceneEdit; a suspended dirty draft refuses edits to its owned entries.",
		UpdateEntries);
	FEATURE_COMMAND("resetSceneEntry",
		"Reset one User entry through its production Reset path, resolving the lower-layer value for weather/location or its original value for interior/time-of-day. Args: sceneSets context args plus index. The entry must belong to the requested period. Persists and reapplies; author overwrite entries cannot be reset here. Active retained-toolbar entries stay unsaved until saveFeatureSceneEdit; a suspended dirty draft refuses edits to its owned entries.",
		ResetEntry);
	FEATURE_COMMAND("removeSceneEntries",
		"Delete User entries, including tombstones, from one requested scene period. Args: sceneSets context args plus indices=[index,...]. Validates every index before removing in descending order. Persists and reapplies; never modifies author pack files. Removing a User override exposes the lower layer. Refresh sceneSets afterward. Active retained-toolbar entries stay unsaved until saveFeatureSceneEdit; a suspended dirty draft refuses edits to its owned entries.",
		RemoveEntries);
	FEATURE_COMMAND("suppressSceneSetting",
		"Persist a User tombstone suppressing this context's value at one setting address. Args: feature, setting, path=string array (empty default), plus sceneSets context args. Lower scene/base layers show through; author pack files stay intact. Remove the tombstone through removeSceneEntries to restore the context value. Active retained-toolbar entries stay unsaved until saveFeatureSceneEdit; a suspended dirty draft refuses edits to its owned entries.",
		SuppressSetting);
	FEATURE_COMMAND("copySceneSettings",
		"Copy compatible winning settings from a whole source context to User entries in a destination context, then persist/reapply. Args: source and destination objects using sceneSets context args, conflicts=skip|overwrite|cancel (skip default). Uses the production grouped-setting, policy and overwrite validation; incompatible settings are skipped. Refuses a copy that reads an unsaved preview or overwrites its dirty context and feature; unrelated copies and saved suspended sources remain available. Preview copySceneCandidates and verify destination sceneSets afterward.",
		CopyContextSettings);
	FEATURE_COMMAND("setLocationTransition",
		"Set location User entries' transition seconds through the production grouped control. Args: type=location, formKey, locationType, period as in sceneSets, indices=[index,...], seconds=finite number clamped to 0..300 or null for global default. Selected entries must belong to the requested period; the owning control can include their aggregate companions. Only supported float settings can change. Persists/reapplies. Verify transitionSeconds with sceneSets. Active retained-toolbar entries stay unsaved until saveFeatureSceneEdit; a suspended dirty draft refuses edits to its owned entries.",
		SetLocationTransition);
	FEATURE_COMMAND("setSceneMode",
		"Persist which saved set a weather or location resolves. Args: type=weather|location, formKey=SPID, timeOfDayEnabled=boolean; location also requires locationType. Does not copy, convert or delete entries. Verify with sceneSets.",
		SetSceneMode);
	FEATURE_RENDER_COMMAND("reloadSceneSettings",
		"Reload personal and active-preset scene files through the production validated reload. Args: none; requires a loaded player cell. Refuses an unsaved retained toolbar draft or unreadable personal document; a clean draft is invalidated. Does not save a toolbar draft. Verify sceneSets and live feature values afterward.",
		ReloadSettings);
}
