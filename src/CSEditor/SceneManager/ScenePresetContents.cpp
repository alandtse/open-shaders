#include "SceneSettingsManager.h"

#include "Presets/PresetContent.h"
#include "SceneSettingsContextRules.h"
#include "SceneSettingsInternal.h"
#include "SceneSettingsLocationTargets.h"
#include "SceneSettingsOverwrites.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"

using namespace SceneSettingsInternal;
using namespace SceneSettingsContextRules;
using namespace SceneSettingsLocationTargets;

std::string SceneSettingsManager::GetSceneExportKey(const SceneContextId& context, const SettingEntry& entry)
{
	return json::array({ static_cast<int>(context.type), static_cast<int>(context.period), context.weatherId,
						   static_cast<int>(context.locationType), context.locationFormKey, entry.featureShortName, entry.settingPath, entry.settingKey })
	    .dump();
}

std::vector<SceneSettingsManager::PresetSceneSetting> SceneSettingsManager::GetPresetSceneSettings(const std::string& packId) const
{
	std::vector<PresetSceneSetting> result;
	FeatureSettingsCache cache;
	std::set<std::string> seen;
	for (const auto& file : PresetContent::List(packId)) {
		try {
			if (!file.document || !PresetContent::IsSceneFile(file.relative))
				continue;
			std::vector<std::string> parts;
			for (const auto& part : file.relative) parts.push_back(Util::PathHelpers::ToUtf8(part));
			SceneContextId context;
			if (parts.front() == "InteriorOnly") {
				if (parts.size() != 2)
					continue;
				context.type = SceneContextType::Interior;
			} else if (parts.front() == "TimeOfDay") {
				if (parts.size() != 3)
					continue;
				context.type = SceneContextType::TimeOfDay;
				context.period = GetPeriodFromName(parts[1]);
				if (context.period == TimeOfDayPeriod::Count)
					continue;
			} else {
				if (parts.size() != 3 && parts.size() != 4)
					continue;
				if (parts.size() == 4) {
					context.period = GetPeriodFromName(parts[2]);
					if (context.period == TimeOfDayPeriod::Count)
						continue;
				}
				if (parts.front() == "Weather") {
					context.type = SceneContextType::Weather;
					context.weatherId = Util::SpidToFormId(parts[1]);
					if (!context.weatherId)
						continue;
				} else {
					context.type = SceneContextType::Location;
					context.locationFormKey = CanonicalizeResolvedLocationFormKey(parts[1]);
					const auto metadata = file.document->value("_metadata", json::object());
					const auto typeName = metadata.value("targetType", std::string{});
					bool found = false;
					for (const auto type : kLocationTargetTypes)
						if ((typeName.empty() || typeName == GetLocationTargetTypeName(type) ||
								(typeName == kLegacyLocationTypeName && type == LocationTargetType::LocationType)) &&
							!ResolveLocationTargetChain(type, context.locationFormKey).empty()) {
							context.locationType = type;
							found = true;
							break;
						}
					if (!found)
						continue;
				}
			}
			const auto rules = GetSceneContextRules(context);
			std::vector<SettingEntry> parsedEntries;
			std::optional<bool> mode;
			if (!SceneSettingsOverwrites::ParseOverwriteFileEntries(*file.document, file.source, rules.sceneType,
					rules.requireNumeric, parsedEntries, &cache, &mode))
				continue;
			for (auto& entry : parsedEntries) {
				entry.period = context.period;
				const auto key = GetSceneExportKey(context, entry);
				if (seen.insert(key).second)
					result.push_back({ key, context, std::move(entry), mode });
			}
		} catch (const std::exception& error) {
			logger::warn("[Presets] Could not read scene file {}: {}", Util::PathHelpers::ToUtf8(file.source), error.what());
		}
	}
	return result;
}

std::vector<SceneSettingsManager::PresetSceneSetting> SceneSettingsManager::GetSceneExportSettings()
{
	std::vector<PresetSceneSetting> result;
	VisitEntryLayers({}, [&](const SceneContextId& target, const std::vector<SettingEntry>& sourceEntries) {
		std::set<TimeOfDayPeriod> periods;
		for (const auto& entry : sourceEntries) periods.insert(entry.period);
		for (const auto period : periods) {
			auto context = target;
			context.period = period;
			for (const auto& [identity, entry] : BuildEffectiveContextEntries(sourceEntries, context, true))
				result.push_back({ GetSceneExportKey(context, *entry), context, *entry, IsSceneTimeOfDayEnabled(context) });
		}
	});
	return result;
}

SceneSettingsManager::CopyResult SceneSettingsManager::CopyPresetSceneSettings(const std::string& packId,
	std::span<const std::string> selected, CopyConflictPolicy policy)
{
	CopyResult result;
	if (HasPendingContextEdit()) {
		result.blockedByPendingEdit = result.cancelled = true;
		return result;
	}
	if (!IsValidCopyConflictPolicy(policy) || selected.empty())
		return result;
	const auto source = GetPresetSceneSettings(packId);
	const auto snapshot = CaptureUserSettings();
	std::map<std::pair<SceneContextId, CopyGroupKey>, std::vector<const PresetSceneSetting*>> groups;
	for (const auto& setting : source)
		groups[{ setting.context, GetCopyGroupKey({ setting.entry.featureShortName, setting.entry.settingPath, setting.entry.settingKey }) }].push_back(&setting);
	std::set<std::string> chosen;
	for (const auto& [group, members] : groups) {
		if (!std::ranges::any_of(members, [&](const auto* setting) { return std::ranges::find(selected, setting->key) != selected.end(); }))
			continue;
		const bool conflicts = std::ranges::any_of(members, [&](const auto* setting) {
			return FindContextUserEntry(setting->context, setting->entry.featureShortName, setting->entry.settingPath, setting->entry.settingKey).has_value();
		});
		result.hadConflicts |= conflicts;
		if (conflicts && policy == CopyConflictPolicy::Cancel) {
			result.cancelled = true;
			return result;
		}
		if (conflicts && policy == CopyConflictPolicy::SkipExisting) {
			result.skipped += members.size();
			continue;
		}
		for (const auto* setting : members) chosen.insert(setting->key);
	}
	for (const auto& setting : source) {
		if (setting.entry.deleted || !chosen.contains(setting.key))
			continue;
		const auto& entry = setting.entry;
		const auto& context = setting.context;
		const auto existing = FindContextUserEntry(context, entry.featureShortName, entry.settingPath, entry.settingKey);
		result.hadConflicts |= existing.has_value();
		if (existing && policy == CopyConflictPolicy::Cancel) {
			RestoreUserSettings(snapshot);
			result.cancelled = true;
			return result;
		}
		if (existing && policy == CopyConflictPolicy::SkipExisting) {
			++result.skipped;
			continue;
		}
		const bool authored = IsSceneContextAuthored(context);
		auto* destination = EnsureContextEntriesMut(context);
		if (!destination) {
			++result.incompatible;
			continue;
		}
		if (existing) {
			auto& target = (*destination)[*existing];
			target.value = entry.value;
			target.paused = false;
			target.deleted = false;
			++result.overwritten;
		} else {
			auto copy = entry;
			const auto base = CaptureContextValue(context, { entry.featureShortName, entry.settingPath, entry.settingKey });
			if (!base) {
				++result.incompatible;
				continue;
			}
			copy.originalValue = *base;
			copy.source = EntrySource::User;
			copy.sourcePath.clear();
			copy.sourceFilename.clear();
			copy.paused = false;
			destination->push_back(std::move(copy));
			++result.copied;
		}
		if (!authored && setting.timeOfDayEnabled)
			RestoreContextUserMode(context, setting.timeOfDayEnabled);
		MarkContextUserSettingsModified(context, true);
		MarkSceneValuesDirty();
	}
	if (!result.Changed())
		return result;
	BumpEntryPresentationRevision();
	if (!WriteUserSettings()) {
		RestoreUserSettings(snapshot);
		result = {};
		result.cancelled = true;
		return result;
	}
	ReapplyIfActive();
	return result;
}
