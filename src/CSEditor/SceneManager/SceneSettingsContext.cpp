#include "SceneSettingsManager.h"

#include "Feature.h"
#include "SceneSettingsContextRules.h"
#include "SceneSettingsInternal.h"
#include "SceneSettingsLocationTargets.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/SettingsCatalog.h"

#include <algorithm>
#include <cassert>
#include <filesystem>

using namespace SceneSettingsInternal;
using namespace SceneSettingsLocationTargets;
using namespace SceneSettingsContextRules;

/** @brief A user entry whose revert baseline is the value it starts with. */
static SceneSettingsManager::SettingEntry MakeUserEntry(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, json value,
	SceneSettingsManager::TimeOfDayPeriod period, bool deleted = false)
{
	return {
		.featureShortName = featureShortName,
		.settingPath = settingPath,
		.settingKey = settingKey,
		.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey),
		.value = value,
		.originalValue = std::move(value),
		.deleted = deleted,
		.source = SceneSettingsManager::EntrySource::User,
		.period = period,
	};
}

/** @brief The user entry at index, whose persisted value may be edited. */
static SceneSettingsManager::SettingEntry* FindUserEntry(std::vector<SceneSettingsManager::SettingEntry>* entries,
	size_t index)
{
	if (!entries || index >= entries->size())
		return nullptr;
	auto& entry = (*entries)[index];
	return entry.source == SceneSettingsManager::EntrySource::User ? &entry : nullptr;
}

bool SceneSettingsManager::IsPeriodicContext(SceneContextType type)
{
	return type != SceneContextType::Interior;
}

bool SceneSettingsManager::IsValidSceneContext(const SceneContextId& context)
{
	const auto periodIndex = static_cast<int>(context.period);
	const bool realPeriod = periodIndex >= 0 && periodIndex < kPeriodCount;
	// Weather and locations keep a flat set (Count) beside their per-period one.
	const bool periodOrFlat = realPeriod || context.period == TimeOfDayPeriod::Count;
	switch (context.type) {
	case SceneContextType::Interior:
		return context.period == TimeOfDayPeriod::Count && context.weatherId == 0 &&
		       context.locationFormKey.empty() && context.locationType == LocationTargetType::Location;
	case SceneContextType::TimeOfDay:
		return realPeriod && context.weatherId == 0 &&
		       context.locationFormKey.empty() && context.locationType == LocationTargetType::Location;
	case SceneContextType::Weather:
		return context.weatherId != 0 && periodOrFlat &&
		       context.locationFormKey.empty() && context.locationType == LocationTargetType::Location;
	case SceneContextType::Location:
		return periodOrFlat && context.weatherId == 0 &&
		       IsValidLocationTargetType(context.locationType) && !context.locationFormKey.empty();
	default:
		return false;
	}
}

bool SceneSettingsManager::IsSameSceneContext(const SceneContextId& lhs, const SceneContextId& rhs)
{
	if (lhs.type != rhs.type)
		return false;
	switch (lhs.type) {
	case SceneContextType::Interior:
		return true;
	case SceneContextType::TimeOfDay:
		return lhs.period == rhs.period;
	case SceneContextType::Weather:
		return lhs.weatherId == rhs.weatherId && lhs.period == rhs.period;
	case SceneContextType::Location:
		return lhs.locationType == rhs.locationType && lhs.period == rhs.period &&
		       NormalizeLocationFormKey(lhs.locationFormKey) == NormalizeLocationFormKey(rhs.locationFormKey);
	default:
		return false;
	}
}

SceneSettingsManager::EffectiveContextEntries SceneSettingsManager::BuildEffectiveContextEntries(
	const std::vector<SettingEntry>& contextEntries, const SceneContextId& context, bool includeDeleted)
{
	EffectiveContextEntries effectiveEntries;
	// User last: a user entry shadows the overwrite it was authored over.
	for (auto entrySource : { EntrySource::Overwrite, EntrySource::User })
		for (const auto& entry : contextEntries)
			if (entry.source == entrySource && !entry.paused && EntryBelongsToContext(entry, context))
				effectiveEntries[{ entry.featureShortName, entry.settingPath, entry.settingKey }] = &entry;
	if (!includeDeleted)
		std::erase_if(effectiveEntries, [](const auto& item) { return item.second->deleted; });
	return effectiveEntries;
}

std::string SceneSettingsManager::GetOverwriteModName(const SettingEntry& entry)
{
	const auto stem = std::filesystem::path(entry.sourceFilename).stem().string();
	const auto lastUnderscore = stem.rfind('_');
	return lastUnderscore == std::string::npos ? stem : stem.substr(0, lastUnderscore);
}

SceneSettingsManager::SettingProvenance SceneSettingsManager::GetSettingProvenance(
	const SceneContextId& context, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey) const
{
	SettingProvenance provenance;
	if (!IsValidSceneContext(context))
		return provenance;

	for (const auto& entry : GetContextEntries(context)) {
		if (!IsEntryActive(entry) || !IsSameSetting(entry, featureShortName, settingPath, settingKey) ||
			!EntryBelongsToContext(entry, context))
			continue;
		if (entry.source == EntrySource::Overwrite) {
			// Discovery order is the entry order, and the last file discovered wins.
			provenance.layer = SettingLayer::Overwrite;
			continue;
		}
		// A user entry outranks every overwrite, so it settles the layer outright.
		provenance.layer = entry.deleted ? SettingLayer::Deleted : SettingLayer::User;
	}
	return provenance;
}

const std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetCopyContextEntries(
	const SceneContextId& context) const
{
	if (!IsValidSceneContext(context))
		return nullptr;
	switch (context.type) {
	case SceneContextType::Interior:
		return &GetEntries(SceneType::InteriorOnly);
	case SceneContextType::TimeOfDay:
		return &GetEntries(SceneType::TimeOfDay);
	case SceneContextType::Weather:
		if (auto configIt = weatherSceneConfigs.find(context.weatherId); configIt != weatherSceneConfigs.end())
			return &configIt->second.entries;
		return nullptr;
	case SceneContextType::Location:
		if (const auto* config = FindLocationConfig(context.locationType, context.locationFormKey))
			return &config->entries;
		return nullptr;
	default:
		return nullptr;
	}
}

std::vector<std::string> SceneSettingsManager::SplitSettingPath(std::string_view catalogPath)
{
	return SplitCatalogPath(catalogPath);
}

std::span<const SceneSettingsManager::SettingEntry> SceneSettingsManager::GetContextEntries(
	const SceneContextId& context) const
{
	const auto* contextEntries = GetCopyContextEntries(context);
	return contextEntries ? std::span{ *contextEntries } : std::span<const SettingEntry>{};
}

std::optional<bool> SceneSettingsManager::GetContextUserMode(const SceneContextId& context) const
{
	if (context.type == SceneContextType::Weather) {
		const auto found = weatherSceneConfigs.find(context.weatherId);
		return found == weatherSceneConfigs.end() ? std::nullopt : found->second.userTimeOfDayEnabled;
	}
	if (context.type == SceneContextType::Location)
		if (const auto* config = FindLocationConfig(context.locationType, context.locationFormKey))
			return config->userTimeOfDayEnabled;
	return std::nullopt;
}

void SceneSettingsManager::RestoreContextUserMode(const SceneContextId& context, std::optional<bool> mode)
{
	if (context.type == SceneContextType::Weather) {
		auto& config = GetWeatherConfigMut(context.weatherId);
		config.userTimeOfDayEnabled = mode;
		config.RefreshTimeOfDayMode();
	} else if (context.type == SceneContextType::Location) {
		auto& config = GetLocationConfigMut(context.locationType, context.locationFormKey);
		config.userTimeOfDayEnabled = mode;
		config.RefreshTimeOfDayMode();
	}
}

bool SceneSettingsManager::BeginContextEdit(const std::string& featureShortName, const SceneContextId& context)
{
	if (!IsValidSceneContext(context) || !Feature::FindFeatureByShortName(featureShortName))
		return false;
	if (contextEdit && (contextEdit->feature != featureShortName || contextEdit->context != context)) {
		if (HasPendingContextEdit())
			return false;
		DiscardContextEdit();
	}
	if (contextEdit && contextEdit->entries.IsActive())
		return true;
	if ((context.type == SceneContextType::Weather && !TryEnsureWeatherDataLoaded()) ||
		(context.type == SceneContextType::Location && !TryEnsureLocationDataLoaded()))
		return false;
	const auto savedDocument = BuildUserSettingsDocument(weatherDataLoaded, locationDataLoaded);
	const bool locationAuthored = context.type == SceneContextType::Location &&
	                              IsLocationTargetAuthored(context.locationType, context.locationFormKey);
	auto* target = EnsureContextEntriesMut(context);
	if (!target)
		return false;
	if (!contextEdit && context.type == SceneContextType::Location)
		GetLocationConfigMut(context.locationType, context.locationFormKey).userAuthored = locationAuthored;
	const auto matches = [&](const SettingEntry& entry) {
		return entry.source == EntrySource::User && entry.featureShortName == featureShortName && entry.period == context.period;
	};
	if (!contextEdit) {
		ContextEdit edit{ .context = context, .feature = featureShortName, .savedMode = GetContextUserMode(context), .workingMode = GetContextUserMode(context), .savedLocationAuthored = locationAuthored, .workingLocationAuthored = locationAuthored, .savedDocument = savedDocument };
		const auto rules = GetSceneContextRules(context);
		for (const auto& metadata : GetCatalogFeatureSettings(featureShortName)) {
			auto path = SplitSettingPath(metadata.settingPath);
			const std::string key{ metadata.settingKey };
			if (IsSettingAllowedForType(rules.sceneType, featureShortName, path, key, rules.requireNumeric))
				edit.previewAddresses.push_back({ featureShortName, std::move(path), key });
		}
		EnsureBaselines(edit.previewAddresses);
		edit.entries.Begin(*target, matches);
		contextEdit = std::move(edit);
	} else {
		contextEdit->entries.Resume(*target, matches);
		RestoreContextUserMode(context, contextEdit->workingMode);
		if (context.type == SceneContextType::Location)
			GetLocationConfigMut(context.locationType, context.locationFormKey).userAuthored = contextEdit->workingLocationAuthored;
		BumpEntryPresentationRevision();
	}
	if ((context.type == SceneContextType::Weather || context.type == SceneContextType::Location) &&
		IsSceneTimeOfDayEnabled(context) != (context.period != TimeOfDayPeriod::Count)) {
		RestoreContextUserMode(context, context.period != TimeOfDayPeriod::Count);
		if (context.type == SceneContextType::Location)
			GetLocationConfigMut(context.locationType, context.locationFormKey).userAuthored = true;
		MarkContextUserSettingsModified(context, false);
	}
	ReapplyIfActive();
	return true;
}

bool SceneSettingsManager::HasPendingContextEdit() const
{
	if (!contextEdit)
		return false;
	const auto& edit = *contextEdit;
	const auto* target = GetCopyContextEntries(edit.context);
	if (!target)
		return false;
	return edit.entries.HasChanges(*target, [&](const SettingEntry& entry) {
		return entry.source == EntrySource::User && entry.featureShortName == edit.feature && entry.period == edit.context.period;
	}) || edit.savedMode != (edit.entries.IsActive() ? GetContextUserMode(edit.context) : edit.workingMode) ||
	       (edit.context.type == SceneContextType::Location && edit.savedLocationAuthored != (edit.entries.IsActive() ? IsLocationTargetAuthored(edit.context.locationType, edit.context.locationFormKey) : edit.workingLocationAuthored));
}

bool SceneSettingsManager::IsContextEditing(std::string_view featureShortName) const
{
	return contextEdit && contextEdit->feature == featureShortName;
}

bool SceneSettingsManager::IsContextEditTarget(const SceneContextId& context, std::string_view featureShortName) const
{
	return contextEdit && contextEdit->feature == featureShortName && IsSameSceneContext(contextEdit->context, context) && HasPendingContextEdit();
}

bool SceneSettingsManager::IsContextEditModeTarget(const SceneContextId& context) const
{
	if (!contextEdit || !HasPendingContextEdit() ||
		(context.type != SceneContextType::Weather && context.type != SceneContextType::Location))
		return false;
	auto selected = context;
	selected.period = contextEdit->context.period;
	return IsSameSceneContext(contextEdit->context, selected);
}

void SceneSettingsManager::SuspendContextEdit()
{
	if (!contextEdit || !contextEdit->entries.IsActive())
		return;
	auto& edit = *contextEdit;
	if (auto* target = GetContextEntriesMut(edit.context)) {
		edit.workingMode = GetContextUserMode(edit.context);
		if (edit.context.type == SceneContextType::Location)
			edit.workingLocationAuthored = IsLocationTargetAuthored(edit.context.locationType, edit.context.locationFormKey);
		edit.entries.Suspend(*target, [&](const SettingEntry& entry) {
			return entry.source == EntrySource::User && entry.featureShortName == edit.feature && entry.period == edit.context.period;
		});
		RestoreContextUserMode(edit.context, edit.savedMode);
		if (edit.context.type == SceneContextType::Location)
			GetLocationConfigMut(edit.context.locationType, edit.context.locationFormKey).userAuthored = edit.savedLocationAuthored;
		BumpEntryPresentationRevision();
		ReapplyIfActive();
	}
}

bool SceneSettingsManager::SaveContextEdit()
{
	if (!contextEdit || !contextEdit->entries.IsActive())
		return false;
	auto& edit = *contextEdit;
	edit.saving = true;
	const SKSE::stl::scope_exit restoreSaving([&edit]() noexcept { edit.saving = false; });
	MarkContextUserSettingsModified(edit.context, false);
	if (!WriteUserSettings())
		return false;
	const auto* target = GetCopyContextEntries(edit.context);
	if (!target)
		return false;
	edit.entries.Accept(*target, [&](const SettingEntry& entry) {
		return entry.source == EntrySource::User && entry.featureShortName == edit.feature && entry.period == edit.context.period;
	});
	edit.savedMode = edit.workingMode = GetContextUserMode(edit.context);
	if (edit.context.type == SceneContextType::Location)
		edit.savedLocationAuthored = edit.workingLocationAuthored = IsLocationTargetAuthored(edit.context.locationType, edit.context.locationFormKey);
	edit.savedDocument = preservedUserSettingsRoot;
	return true;
}

void SceneSettingsManager::DiscardContextEdit()
{
	if (!contextEdit)
		return;
	SuspendContextEdit();
	contextEdit.reset();
	Util::EnvironmentControls::StopPreview();
}

std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetContextEntriesMut(
	const SceneContextId& context)
{
	if (!IsValidSceneContext(context))
		return nullptr;
	switch (context.type) {
	case SceneContextType::Interior:
	case SceneContextType::TimeOfDay:
		return &GetEntriesMut(ContextSceneType(context.type));
	case SceneContextType::Weather:
		if (!TryEnsureWeatherDataLoaded())
			return nullptr;
		if (auto configIt = weatherSceneConfigs.find(context.weatherId); configIt != weatherSceneConfigs.end())
			return &configIt->second.entries;
		return nullptr;
	case SceneContextType::Location:
		if (!TryEnsureLocationDataLoaded())
			return nullptr;
		if (auto* config = FindLocationConfig(context.locationType, context.locationFormKey))
			return &config->entries;
		return nullptr;
	default:
		return nullptr;
	}
}

std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::EnsureContextEntriesMut(
	const SceneContextId& context)
{
	if (!IsValidSceneContext(context))
		return nullptr;
	switch (context.type) {
	case SceneContextType::Weather:
		if (!TryEnsureWeatherDataLoaded())
			return nullptr;
		return &GetWeatherConfigMut(context.weatherId).entries;
	case SceneContextType::Location:
		{
			if (!TryEnsureLocationDataLoaded())
				return nullptr;
			// Adding the first setting authors the target, so the page survives a reload with no settings.
			const auto& existing = GetLocationConfig(context.locationType, context.locationFormKey);
			const auto name = existing.name;
			const auto cocCode = existing.cocCode;
			auto& config = EnsureAuthoredLocationConfig(context.locationType, context.locationFormKey,
				name, cocCode);
			return &config.entries;
		}
	default:
		return GetContextEntriesMut(context);
	}
}

void SceneSettingsManager::MarkContextUserSettingsModified(const SceneContextId& context,
	bool replaceMalformedEntries)
{
	switch (context.type) {
	case SceneContextType::Interior:
	case SceneContextType::TimeOfDay:
		MarkEntryListUserSettingsModified(ContextSceneType(context.type));
		break;
	case SceneContextType::Weather:
		PrepareWeatherUserSettingsMutation(context.weatherId, replaceMalformedEntries);
		break;
	case SceneContextType::Location:
		PrepareLocationUserSettingsMutation(context.locationType, context.locationFormKey,
			replaceMalformedEntries);
		break;
	default:
		break;
	}
}

void SceneSettingsManager::CommitContextUserEntryMutation(const SceneContextId& context, bool deferSave,
	bool replaceMalformedEntries)
{
	BumpEntryPresentationRevision();
	MarkContextUserSettingsModified(context, replaceMalformedEntries);
	// A deferred edit holds the resolve with the save, so a drag does not run ResolveAndApply
	// mid-DrawSettings for every feature holding an active scene override.
	if (deferSave)
		MarkDeferredSceneChanges();
	else
		CommitSceneSettingChanges();
}

std::optional<json> SceneSettingsManager::CaptureContextValue(const SceneContextId& context,
	const SettingAddress& address)
{
	switch (context.type) {
	case SceneContextType::Weather:
		// Weather and location stack on lower layers, so a capture pins what applies without the mods.
		if (const auto lower = ResolveWeatherLowerValue(context.weatherId, address, context.period,
				kCaptureSourceLayer))
			return json(*lower);
		return std::nullopt;
	case SceneContextType::Location:
		return ResolveLocationLowerValue(context.locationType, context.locationFormKey, address,
			kCaptureSourceLayer);
	default:
		{
			// The entry lists sit directly on the feature's own value. Read from the baselines, since
			// suspending the layer to read it would restore and reapply every scene-driven feature.
			auto value = GetBaselineValue(address);
			return value.is_null() ? std::nullopt : std::optional{ std::move(value) };
		}
	}
}

std::optional<json> SceneSettingsManager::ResolveContextEntryDefault(const SceneContextId& context,
	const SettingEntry& entry)
{
	switch (context.type) {
	case SceneContextType::Weather:
		// Re-resolved rather than remembered: the layers under the entry can have moved since.
		if (const auto lower = ResolveWeatherLowerValue(context.weatherId, GetEntryAddress(entry),
				entry.period, entry.source))
			return json(*lower);
		return std::nullopt;
	case SceneContextType::Location:
		return ResolveLocationLowerValue(context.locationType, context.locationFormKey,
			GetEntryAddress(entry), entry.source);
	default:
		return entry.originalValue.is_null() ? std::nullopt : std::optional{ entry.originalValue };
	}
}

SceneSettingsManager::ContextEntrySummary SceneSettingsManager::GetFeatureSceneSummary(
	std::string_view featureShortName, const SceneContextId& context, bool matchPeriod) const
{
	ContextEntrySummary summary;
	const bool periodic = IsSceneTimeOfDayEnabled(context);
	for (const auto& entry : GetContextEntries(context)) {
		if ((!featureShortName.empty() && entry.featureShortName != featureShortName) ||
			(matchPeriod && !EntryBelongsToContext(entry, context)))
			continue;
		++summary.total;
		if (entry.paused)
			++summary.paused;
		if (entry.source == EntrySource::Overwrite && !entry.paused && !entry.deleted &&
			(context.type == SceneContextType::Interior || (entry.period != TimeOfDayPeriod::Count) == periodic))
			++summary.activeOverwrites;
	}
	return summary;
}

SceneSettingsManager::ContextEntrySummary SceneSettingsManager::GetFeatureSceneSummary(
	std::string_view featureShortName, SceneContextType type) const
{
	ContextEntrySummary summary;
	const auto collect = [&](const SceneContextId& context) {
		const auto target = GetFeatureSceneSummary(featureShortName, context, false);
		summary.total += target.total;
		summary.paused += target.paused;
		summary.activeOverwrites += target.activeOverwrites;
	};
	switch (type) {
	case SceneContextType::Interior:
	case SceneContextType::TimeOfDay:
		collect({ .type = type });
		break;
	case SceneContextType::Weather:
		for (const auto& [weatherId, _] : weatherSceneConfigs)
			collect({ .type = type, .weatherId = weatherId });
		break;
	case SceneContextType::Location:
		for (const auto& [_, config] : locationSceneConfigs)
			collect({ .type = type, .locationType = config.type, .locationFormKey = config.formKey });
		break;
	}
	return summary;
}

SceneSettingsManager::ContextEntrySummary SceneSettingsManager::GetContextUserEntrySummary(
	const SceneContextId& context, std::string_view featureShortName) const
{
	ContextEntrySummary summary;
	if (!IsValidSceneContext(context))
		return summary;
	for (const auto& entry : GetContextEntries(context)) {
		if ((!featureShortName.empty() && entry.featureShortName != featureShortName) ||
			entry.source != EntrySource::User || !EntryBelongsToContext(entry, context))
			continue;
		++summary.total;
		summary.paused += entry.paused ? 1 : 0;
	}
	return summary;
}

void SceneSettingsManager::SetContextEntriesPaused(const SceneContextId& context, bool paused, std::string_view featureShortName)
{
	auto* contextEntries = GetContextEntriesMut(context);
	if (!contextEntries)
		return;

	bool changed = false;
	for (auto& entry : *contextEntries) {
		if ((!featureShortName.empty() && entry.featureShortName != featureShortName) ||
			entry.source != EntrySource::User || entry.paused == paused ||
			!EntryBelongsToContext(entry, context))
			continue;
		entry.paused = paused;
		changed = true;
	}
	if (changed)
		CommitContextUserEntryMutation(context);
}

void SceneSettingsManager::ClearContextEntries(const SceneContextId& context, std::string_view featureShortName)
{
	if (contextEdit && contextEdit->context == context && (featureShortName.empty() || featureShortName == contextEdit->feature))
		DiscardContextEdit();
	auto* contextEntries = GetContextEntriesMut(context);
	if (!contextEntries)
		return;

	// Unresolved raw entries are left in the document: they belong to features this session cannot judge.
	const auto removed = std::erase_if(*contextEntries, [&](const SettingEntry& entry) {
		return (featureShortName.empty() || entry.featureShortName == featureShortName) &&
		       entry.source == EntrySource::User && EntryBelongsToContext(entry, context);
	});
	if (removed != 0)
		CommitContextUserEntryMutation(context);
}

bool SceneSettingsManager::HasAnyUserEntries() const
{
	const auto hasUser = [](const std::vector<SettingEntry>& sourceEntries) {
		return std::any_of(sourceEntries.begin(), sourceEntries.end(),
			[](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	};
	return std::any_of(entries.begin(), entries.end(),
			   [&](const auto& item) { return hasUser(item.second); }) ||
	       std::any_of(weatherSceneConfigs.begin(), weatherSceneConfigs.end(),
			   [&](const auto& item) { return hasUser(item.second.entries); }) ||
	       std::any_of(locationSceneConfigs.begin(), locationSceneConfigs.end(),
			   [&](const auto& item) { return hasUser(item.second.entries); });
}

void SceneSettingsManager::ClearAllUserEntries()
{
	DiscardContextEdit();
	// Unresolved raw entries stay in the document: they belong to features this session cannot judge.
	bool changed = false;
	for (auto& [type, sourceEntries] : entries)
		if (IsEntryListSceneType(type) && EraseUserEntries(sourceEntries) != 0) {
			MarkEntryListUserSettingsModified(type);
			changed = true;
		}
	for (auto& [weatherId, config] : weatherSceneConfigs)
		if (EraseUserEntries(config.entries) != 0) {
			PrepareWeatherUserSettingsMutation(weatherId, false);
			changed = true;
		}
	for (auto& [configKey, config] : locationSceneConfigs)
		if (EraseUserEntries(config.entries) != 0) {
			locationUserSettingsModified = true;
			changed = true;
		}

	if (!changed)
		return;
	BumpEntryPresentationRevision();
	CommitSceneSettingChanges();
}

bool SceneSettingsManager::VisitEntryLayers(const EntryLayerScope& scope,
	const std::function<void(const SceneContextId&, std::vector<SettingEntry>&)>& visit)
{
	if ((!scope.type || *scope.type == SceneContextType::Weather) && !TryEnsureWeatherDataLoaded())
		return false;
	if ((!scope.type || *scope.type == SceneContextType::Location) && !TryEnsureLocationDataLoaded())
		return false;
	const auto visitMatching = [&](SceneContextId context, std::vector<SettingEntry>& sourceEntries) {
		if (scope.type && *scope.type != context.type)
			return;
		if (scope.target) {
			auto target = *scope.target;
			target.period = context.period;
			if (!IsSameSceneContext(context, target))
				return;
		}
		visit(context, sourceEntries);
	};
	visitMatching({ .type = SceneContextType::Interior }, GetEntriesMut(SceneType::InteriorOnly));
	visitMatching({ .type = SceneContextType::TimeOfDay }, GetEntriesMut(SceneType::TimeOfDay));
	for (auto& [weatherId, config] : weatherSceneConfigs)
		visitMatching({ .type = SceneContextType::Weather, .weatherId = weatherId }, config.entries);
	for (auto& [key, config] : locationSceneConfigs)
		visitMatching({ .type = SceneContextType::Location, .locationType = config.type, .locationFormKey = config.formKey }, config.entries);
	return true;
}

SceneSettingsManager::ContextEntrySummary SceneSettingsManager::GetEntryLayerSummary(
	EntrySource source, const EntryLayerScope& scope)
{
	ContextEntrySummary summary;
	VisitEntryLayers(scope, [&](const SceneContextId&, const std::vector<SettingEntry>& sourceEntries) {
		for (const auto& entry : sourceEntries)
			if (entry.source == source) {
				++summary.total;
				summary.paused += entry.paused ? 1 : 0;
			}
	});
	return summary;
}

void SceneSettingsManager::SetEntryLayerPaused(EntrySource source, bool paused, const EntryLayerScope& scope)
{
	if (HasPendingContextEdit())
		return;
	DiscardContextEdit();
	bool changed = false;
	VisitEntryLayers(scope, [&](const SceneContextId& context, std::vector<SettingEntry>& sourceEntries) {
		bool listChanged = false;
		for (auto& entry : sourceEntries)
			if (entry.source == source && entry.paused != paused) {
				entry.paused = paused;
				listChanged = true;
			}
		if (listChanged && source == EntrySource::User)
			MarkContextUserSettingsModified(context, false);
		changed |= listChanged;
	});
	if (changed) {
		BumpEntryPresentationRevision();
		if (source == EntrySource::User)
			SaveAllUserSettings();
		ReapplyIfActive();
	}
}

SceneSettingsManager::EntryLayerFileResult SceneSettingsManager::DeleteEntryLayer(
	EntrySource source, const EntryLayerScope& scope)
{
	EntryLayerFileResult result;
	if (HasPendingContextEdit()) {
		result.error = EntryLayerFileResult::Error::PendingEdit;
		return result;
	}
	DiscardContextEdit();
	std::set<std::filesystem::path> files;
	bool changed = false;
	const bool loaded = VisitEntryLayers(scope, [&](const SceneContextId& context, std::vector<SettingEntry>& sourceEntries) {
		if (source == EntrySource::Overwrite) {
			for (const auto& entry : sourceEntries)
				if (entry.source == source && !entry.sourcePath.empty())
					files.insert(entry.sourcePath);
		} else if (EraseUserEntries(sourceEntries)) {
			MarkContextUserSettingsModified(context, false);
			changed = true;
		}
	});
	if (!loaded) {
		result.error = EntryLayerFileResult::Error::DataUnavailable;
		return result;
	}
	const auto roots = GetSceneOverwriteRoots();
	for (const auto& path : files) {
		std::error_code error;
		const bool owned = std::ranges::any_of(roots, [&](const auto& root) { return Util::IsPathWithinDirectory(root, path); });
		if (owned && std::filesystem::remove(path, error))
			++result.written;
		else {
			++result.failed;
			logger::error("[SceneSettings] Could not delete loaded overwrite '{}': {}", Util::PathHelpers::ToUtf8(path),
				owned ? error.message() : "path is outside the scene roots");
		}
	}
	if (source == EntrySource::Overwrite && result.written)
		ReloadOverwrites();
	if (changed) {
		BumpEntryPresentationRevision();
		CommitSceneSettingChanges();
	}
	return result;
}

std::optional<size_t> SceneSettingsManager::FindContextUserEntry(const SceneContextId& context,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey) const
{
	if (!IsValidSceneContext(context))
		return std::nullopt;
	const auto contextEntries = GetContextEntries(context);
	for (size_t index = 0; index < contextEntries.size(); ++index) {
		const auto& entry = contextEntries[index];
		if (entry.source == EntrySource::User && entry.featureShortName == featureShortName &&
			entry.settingPath == settingPath && entry.settingKey == settingKey &&
			entry.period == context.period)
			return index;
	}
	return std::nullopt;
}

std::optional<size_t> SceneSettingsManager::AddContextSetting(const SceneContextId& context,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey, bool deferSave)
{
	if (!IsValidSceneContext(context))
		return std::nullopt;

	const auto rules = GetSceneContextRules(context);
	if (!IsSettingAllowedForType(rules.sceneType, featureShortName, settingPath, settingKey, rules.requireNumeric) ||
		FindContextUserEntry(context, featureShortName, settingPath, settingKey))
		return std::nullopt;

	auto value = CaptureContextValue(context, { featureShortName, settingPath, settingKey });
	if (!value || !ValidateSceneSettingEntry(rules.label, rules.sceneType, featureShortName, settingPath,
					  settingKey, *value, rules.requireNumeric))
		return std::nullopt;

	auto* contextEntries = EnsureContextEntriesMut(context);
	if (!contextEntries)
		return std::nullopt;

	contextEntries->push_back(
		MakeUserEntry(featureShortName, settingPath, settingKey, std::move(*value), context.period));

	CommitContextUserEntryMutation(context, deferSave);
	return FindContextUserEntry(context, featureShortName, settingPath, settingKey);
}

void SceneSettingsManager::UpdateContextEntryValues(const SceneContextId& context,
	std::span<const EntryValueUpdate> updates, bool deferSave)
{
	auto* contextEntries = GetContextEntriesMut(context);
	if (!contextEntries)
		return;

	const auto rules = GetSceneContextRules(context);
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(rules.label, rules.sceneType, *contextEntries, updates, rules.requireNumeric,
			userEntriesChanged))
		return;
	if (userEntriesChanged)
		MarkContextUserSettingsModified(context, false);

	// Only values moved, so the presentation caches still hold. The value caches must still go, and
	// the next Update resolves them so a drag previews live without resolving mid-DrawSettings.
	if (deferSave) {
		resolverDirty = true;
		MarkSceneValuesDirty();
		MarkDeferredSceneChanges();
		return;
	}
	if (userEntriesChanged)
		SaveAllUserSettings();
	ReapplyIfActive();
}

bool SceneSettingsManager::ApplyReplicaSettingsChanges(Feature& feature, const SceneContextId& context,
	const json& originalSettings, const json& changedSettings)
{
	if (!IsValidSceneContext(context))
		return false;
	const auto featureName = feature.GetShortName();
	const auto rules = GetSceneContextRules(context);
	const auto sourceEntries = GetContextEntries(context);
	std::vector<SettingEntry> nextEntries(sourceEntries.begin(), sourceEntries.end());
	std::vector<EntryValueUpdate> updates;
	for (const auto& setting : GetCatalogFeatureSettings(featureName)) {
		const auto* previous = GetCatalogSerializedValue(originalSettings, setting);
		const auto* changed = GetCatalogSerializedValue(changedSettings, setting);
		if (!previous || !changed || AppliedValuesEqual(*previous, *changed))
			continue;
		const auto path = SplitSettingPath(setting.settingPath);
		const std::string key{ setting.settingKey };
		if (!IsSettingAllowedForType(rules.sceneType, featureName, path, key, rules.requireNumeric) ||
			!IsSceneSettingValueAllowed(*previous, setting, *changed, rules.requireNumeric))
			continue;
		auto entry = std::ranges::find_if(nextEntries, [&](const SettingEntry& candidate) {
			return candidate.source == EntrySource::User && candidate.featureShortName == featureName &&
			       candidate.settingPath == path && candidate.settingKey == key && EntryBelongsToContext(candidate, context);
		});
		if (entry == nextEntries.end()) {
			auto captured = CaptureContextValue(context, { featureName, path, key });
			if (!captured)
				return false;
			nextEntries.push_back(MakeUserEntry(featureName, path, key, std::move(*captured), context.period));
			entry = std::prev(nextEntries.end());
		}
		entry->deleted = false;
		updates.push_back({ static_cast<size_t>(std::distance(nextEntries.begin(), entry)), *changed });
	}
	if (updates.empty())
		return true;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(rules.label, rules.sceneType, nextEntries, updates, rules.requireNumeric, userEntriesChanged))
		return false;
	auto* destination = EnsureContextEntriesMut(context);
	if (!destination)
		return false;
	auto previousEntries = std::move(*destination);
	*destination = std::move(nextEntries);
	try {
		CommitContextUserEntryMutation(context, true);
	} catch (...) {
		*destination = std::move(previousEntries);
		BumpEntryPresentationRevision();
		throw;
	}
	return true;
}

void SceneSettingsManager::RemoveContextSetting(const SceneContextId& context, size_t index)
{
	if (!IsValidSceneContext(context))
		return;

	// A mod's file is never edited from here. Suppressing a mod value goes through TombstoneContextSetting.
	const auto contextEntries = GetContextEntries(context);
	if (index >= contextEntries.size())
		return;
	if (contextEntries[index].source != EntrySource::User) {
		assert(false && "RemoveContextSetting called with an overwrite entry");
		logger::warn("[SceneSettings] Refusing to remove overwrite entry {} through the context façade",
			GetSettingLogName(contextEntries[index].featureShortName,
				contextEntries[index].settingPath, contextEntries[index].settingKey));
		return;
	}

	switch (context.type) {
	case SceneContextType::Interior:
	case SceneContextType::TimeOfDay:
		RemoveSetting(ContextSceneType(context.type), index);
		break;
	case SceneContextType::Weather:
		RemoveWeatherSetting(context.weatherId, index);
		break;
	case SceneContextType::Location:
		RemoveLocationSetting(context.locationType, context.locationFormKey, index);
		break;
	default:
		break;
	}
}

bool SceneSettingsManager::TombstoneContextSetting(const SceneContextId& context,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey)
{
	auto* contextEntries = GetContextEntriesMut(context);
	if (!contextEntries)
		return false;

	// A tombstone must not land at an address where an ordinary user entry would be rejected.
	const auto rules = GetSceneContextRules(context);
	if (!IsSettingAllowedForType(rules.sceneType, featureShortName, settingPath, settingKey, rules.requireNumeric))
		return false;

	// An existing user entry becomes the tombstone: two entries at one address would race.
	if (const auto existing = FindContextUserEntry(context, featureShortName, settingPath, settingKey)) {
		auto& entry = (*contextEntries)[*existing];
		entry.deleted = true;
		entry.paused = false;
		CommitContextUserEntryMutation(context);
		return true;
	}

	if (GetSettingProvenance(context, featureShortName, settingPath, settingKey).layer == SettingLayer::None)
		return false;
	// The base is kept for the round trip and for revert; resolution ignores it while deleted. A null
	// one would not survive a reload, so the address stays as it is rather than losing the tombstone.
	auto value = GetBaselineValue({ featureShortName, settingPath, settingKey });
	if (value.is_null())
		return false;

	contextEntries->push_back(
		MakeUserEntry(featureShortName, settingPath, settingKey, std::move(value), context.period, true));
	CommitContextUserEntryMutation(context);
	return true;
}

void SceneSettingsManager::ClearContextTombstone(const SceneContextId& context,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey)
{
	auto* contextEntries = GetContextEntriesMut(context);
	if (!contextEntries)
		return;
	const auto existing = FindContextUserEntry(context, featureShortName, settingPath, settingKey);
	if (!existing || *existing >= contextEntries->size() || !(*contextEntries)[*existing].deleted)
		return;
	contextEntries->erase(contextEntries->begin() + static_cast<ptrdiff_t>(*existing));
	CommitContextUserEntryMutation(context);
}

void SceneSettingsManager::TogglePauseContextEntry(const SceneContextId& context, size_t index)
{
	auto* contextEntries = GetContextEntriesMut(context);
	if (!contextEntries || index >= contextEntries->size())
		return;
	auto& entry = (*contextEntries)[index];
	if (entry.source == EntrySource::Overwrite && HasPendingContextEdit())
		return;
	entry.paused = !entry.paused;
	if (entry.source == EntrySource::User)
		CommitContextUserEntryMutation(context);
	else {
		BumpEntryPresentationRevision();
		ReapplyIfActive();
	}
}

void SceneSettingsManager::RevertContextEntryToDefault(const SceneContextId& context, size_t index)
{
	// Rewriting a mod's entry would diverge it from its backing file and lose the edit on the next
	// discovery, so it is refused here as it already is in bulk.
	auto* userEntry = FindUserEntry(GetContextEntriesMut(context), index);
	if (!userEntry)
		return;
	auto& entry = *userEntry;

	const auto rules = GetSceneContextRules(context);
	const auto defaultValue = ResolveContextEntryDefault(context, entry);
	if (!defaultValue || !ValidateSceneSettingEntry(rules.label, rules.sceneType, entry.featureShortName,
							 entry.settingPath, entry.settingKey, *defaultValue, rules.requireNumeric))
		return;

	entry.value = *defaultValue;
	entry.originalValue = entry.value;
	// Only the value moved, so the presentation caches hold: no revision bump, unlike a mutation.
	MarkContextUserSettingsModified(context, false);
	CommitSceneSettingChanges();
}
