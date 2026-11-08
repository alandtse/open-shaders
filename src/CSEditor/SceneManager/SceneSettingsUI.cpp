#include "SceneSettingsUI.h"
#include "SceneSettingsContextRules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../../I18n/I18n.h"
#include "../Browser/BrowserWidgets.h"
#include "../EditorWindow.h"
#include "IconsTabler.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/Icons/helpers/SceneActionIcons.h"
#include "Menu/Presets/PresetExportPanel.h"
#include "SceneCopyModal.h"
#include "SceneEntryTable.h"
#include "SceneFeatureReplica.h"
#include "SceneManager.h"
#include "State.h"
#include "Utils/Form.h"
#include "Utils/Game.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#include <imgui_stdlib.h>

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using TimeOfDayPeriod = SceneSettingsManager::TimeOfDayPeriod;
	constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;
	constexpr float kManageModalWidthEm = 38.0f;
	constexpr float kCopyModalWidthEm = 48.0f;
	constexpr float kTargetManageWidthEm = 24.0f;
	constexpr float kSceneTargetPopupWidthEm = 32.0f;
	constexpr ImVec4 kCurrentSceneColor(0.77f, 0.60f, 0.95f, 1.0f);
	std::string featurePageName;
	std::optional<SceneSettingsManager::SceneContextId> featurePageContext;
	bool featureToolbarOpen = false;
	bool featureSaveFailed = false;
	std::optional<std::pair<std::string, SceneSettingsManager::SceneContextId>> requestedFeatureContext;
	Util::ConfirmationPopup featureContextConfirmation;
	Util::ConfirmationPopup featureDeleteConfirmation;
	Util::DeferredModal manageSettingsModal;
	bool targetManagementRequested = false;
	std::pair<SceneSettingsManager::SceneContextId, std::string> managedTarget;
	Util::ConfirmationPopup deleteSceneChangesConfirmation;
	SceneSettingsManager::EntryLayerScope pendingDeleteScope;
	int manageSection = 0;
	SceneSettingsManager::EntrySource pendingDeleteSource = SceneSettingsManager::EntrySource::User;
	SceneSettingsManager::SettingSelection featurePageSelection;
	std::optional<std::vector<std::string>> featurePageKeys;
	std::uint64_t featurePageScopeRevision = 0;
	int standalonePage = 0;
	RE::FormID standaloneWeather = 0;
	std::optional<SceneSettingsManager::LocationTarget> standaloneLocation;

	template <class Action>
	void ChooseFeatureActionLayer(const char* id, bool pressed, bool hasUser, bool hasPreset, bool presetBlocked, Action&& action)
	{
		using Source = SceneSettingsManager::EntrySource;
		if (pressed) {
			if (hasUser && hasPreset)
				ImGui::OpenPopup(id);
			else if (hasUser || (hasPreset && !presetBlocked))
				action(hasUser ? Source::User : Source::Overwrite);
		}
		if (!ImGui::BeginPopup(id))
			return;
		if (Util::RoundedMenuItem(T("feature.scene_manager.section.user_settings", "User Settings"), nullptr, false, hasUser))
			action(Source::User);
		if (Util::RoundedMenuItem(T("feature.scene_manager.section.overwrite_files", "Preset Settings"), nullptr, false, hasPreset && !presetBlocked))
			action(Source::Overwrite);
		ImGui::EndPopup();
	}

	const char* GetPeriodLabel(int period)
	{
		switch (static_cast<TimeOfDayPeriod>(period)) {
		case TimeOfDayPeriod::Dawn:
			return T(TKEY("tod_dawn"), "Dawn");
		case TimeOfDayPeriod::Sunrise:
			return T(TKEY("tod_sunrise"), "Sunrise");
		case TimeOfDayPeriod::Day:
			return T(TKEY("tod_day"), "Day");
		case TimeOfDayPeriod::Sunset:
			return T(TKEY("tod_sunset"), "Sunset");
		case TimeOfDayPeriod::Dusk:
			return T(TKEY("tod_dusk"), "Dusk");
		default:
			return T(TKEY("tod_night"), "Night");
		}
	}

	/// Scene-capable features, resolved once: loaded features and the catalog are fixed after boot.
	/// Transitionable-only is the weather and time-of-day set; the rest also covers interior and location.
	const std::vector<Feature*>& GetSceneFeatures(bool transitionableOnly)
	{
		auto build = [](const std::vector<std::string>& names) {
			std::vector<Feature*> features;
			features.reserve(names.size());
			for (const auto& name : names) {
				if (auto* feature = Feature::FindFeatureByShortName(name))
					features.push_back(feature);
			}
			return features;
		};
		static const std::vector<Feature*> transitionable = build(SceneSettingsManager::GetExteriorRelevantFeatureNames());
		static const std::vector<Feature*> all = build(SceneSettingsManager::GetLocationRelevantFeatureNames());
		return transitionableOnly ? transitionable : all;
	}

	const char* GetLocationTypeLabel(SceneSettingsManager::LocationTargetType type)
	{
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Worldspace:
			return T(TKEY("location_type_worldspace"), "Worldspace");
		case SceneSettingsManager::LocationTargetType::LocationType:
			return T(TKEY("location_type_location_type"), "Location Type");
		case SceneSettingsManager::LocationTargetType::Region:
			return T(TKEY("location_type_region"), "Region");
		case SceneSettingsManager::LocationTargetType::Cell:
			return T(TKEY("location_type_cell"), "Cell");
		default:
			return T(TKEY("location_type_location"), "Location");
		}
	}

	bool IsSameLocationTarget(const SceneSettingsManager::LocationTarget& lhs, const SceneSettingsManager::LocationTarget& rhs)
	{
		return lhs.type == rhs.type && lhs.formKey == rhs.formKey;
	}

	/// The form key is the fallback identity for targets whose editor ID the game does not expose.
	const std::string& GetLocationIdentityText(const SceneSettingsManager::LocationTarget& target)
	{
		return target.editorId.empty() ? target.formKey : target.editorId;
	}

	bool StartFeatureContext(const std::string& name, const SceneSettingsManager::SceneContextId& context)
	{
		if (!SceneSettingsManager::GetSingleton()->BeginContextEdit(name, context))
			return false;
		const bool retargetPreview = featurePageName == name && featureToolbarOpen && Util::EnvironmentControls::IsPreviewActive();
		if (!retargetPreview)
			Util::EnvironmentControls::StopPreview();
		featurePageName = name;
		featurePageContext = context;
		featureToolbarOpen = true;
		featureSaveFailed = false;
		if (retargetPreview && !SceneSettingsUI::SetFeaturePagePreviewPlaying(true))
			Util::EnvironmentControls::StopPreview();
		return true;
	}

	void RequestFeatureContext(const std::string& name, const SceneSettingsManager::SceneContextId& context)
	{
		if (featurePageName == name && featurePageContext == context && featureToolbarOpen &&
			SceneSettingsManager::GetSingleton()->IsContextEditing(name))
			return;
		if (SceneSettingsManager::GetSingleton()->HasPendingContextEdit()) {
			requestedFeatureContext = { name, context };
			featureContextConfirmation.title = T("feature.scene_manager.edit.discard_title", "Discard Unsaved Scene Settings?");
			featureContextConfirmation.message = T("feature.scene_manager.edit.discard_scene_message",
				"Unsaved scene settings will be discarded. Switch to the selected scene?");
			featureContextConfirmation.confirmLabel = T("feature.scene_manager.edit.discard_switch_confirm", "Discard and Switch");
			featureContextConfirmation.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
			featureContextConfirmation.Request();
			return;
		}
		StartFeatureContext(name, context);
	}

	void DrawFeatureContextConfirmation(const std::string& name)
	{
		if (!requestedFeatureContext || requestedFeatureContext->first != name)
			return;
		if (featureContextConfirmation.Draw()) {
			SceneSettingsManager::GetSingleton()->DiscardContextEdit();
			StartFeatureContext(name, requestedFeatureContext->second);
			requestedFeatureContext.reset();
		} else if (!featureContextConfirmation.IsOpen()) {
			requestedFeatureContext.reset();
		}
	}

	std::optional<SceneSettingsManager::SceneContextId> InitialFeatureContext(
		SceneSettingsManager::SceneContextType type, const std::string& name)
	{
		auto context = SceneSettingsUI::ResolveCurrentLayer(type, name);
		if (!context && type == SceneSettingsManager::SceneContextType::Location &&
			SceneSettingsUI::LayerListsFeature(type, name)) {
			const auto& targets = SceneSettingsManager::GetSingleton()->GetCurrentLocationTargets();
			if (!targets.empty()) {
				const auto& target = targets.back();
				context = SceneSettingsManager::SceneContextId{ .type = type, .locationType = target.type, .locationFormKey = target.formKey };
			}
		}
		if (context && type != SceneSettingsManager::SceneContextType::Interior &&
			SceneSettingsManager::GetSingleton()->IsSceneTimeOfDayEnabled(*context))
			context->period = SceneSettingsManager::GetCurrentPeriod();
		return context;
	}

	enum class SceneSettingMarker
	{
		None,
		Settings,
		Overwrite
	};

	SceneSettingMarker GetFeatureSceneMarker(std::string_view feature, const SceneSettingsManager::SceneContextId& context,
		bool matchPeriod = true, bool wholeType = false)
	{
		if (feature.empty())
			return SceneSettingMarker::None;
		auto* manager = SceneSettingsManager::GetSingleton();
		static std::string cachedFeature;
		static std::uint64_t cachedScopeRevision = 0;
		static std::uint64_t cachedRevision = std::numeric_limits<std::uint64_t>::max();
		static std::map<std::tuple<SceneSettingsManager::SceneContextId, bool, bool>, SceneSettingMarker> markers;
		if (cachedFeature != feature || cachedRevision != manager->GetEntryPresentationRevision() || cachedScopeRevision != featurePageScopeRevision) {
			cachedFeature = feature;
			cachedScopeRevision = featurePageScopeRevision;
			cachedRevision = manager->GetEntryPresentationRevision();
			markers.clear();
		}
		auto [it, inserted] = markers.try_emplace(std::tuple{ context, matchPeriod, wholeType });
		if (inserted) {
			static const SceneSettingsManager::SettingSelection allSettings;
			const auto& selection = featureToolbarOpen && feature == featurePageName ? featurePageSelection : allSettings;
			const auto summary = wholeType ? manager->GetFeatureSceneSummary(feature, context.type, selection) :
			                                 manager->GetFeatureSceneSummary(feature, context, matchPeriod, selection);
			it->second = summary.presetSettings ? SceneSettingMarker::Overwrite :
			             summary.total          ? SceneSettingMarker::Settings :
			                                      SceneSettingMarker::None;
		}
		return it->second;
	}

	void DrawFeatureFilter(std::string_view feature, bool& filter)
	{
		const bool scoped = featurePageSelection.has_value() && feature == featurePageName;
		ImGui::Checkbox(scoped ? T(TKEY("scene_feature_tab_specific"), "Feature Specific (Tab)") :
								 T("feature.scene_manager.edit.configured_only", "Feature Specific"),
			&filter);
		Util::AddTooltip(scoped ? T(TKEY("scene_feature_tab_specific_tooltip"), "Only show scenes with saved settings for this feature tab.") :
								  T("feature.scene_manager.edit.configured_only.tooltip", "Only show scenes with saved settings for this feature."));
	}

	ImVec4 GetScenePickerTextColor(bool current, SceneSettingMarker marker)
	{
		if (current)
			return kCurrentSceneColor;
		if (marker == SceneSettingMarker::Overwrite)
			return Util::Colors::GetWarning();
		if (marker == SceneSettingMarker::Settings)
			return Util::Colors::GetSuccess();
		return ImGui::GetStyleColorVec4(ImGuiCol_Text);
	}

	void ReportScenePickerHint(bool current, SceneSettingMarker marker)
	{
		if (!ImGui::IsItemHovered(Util::kTooltipWhenDisabled))
			return;
		if (current)
			FeatureListRenderer::SetSettingHint(T(TKEY("scene_picker_current_hint"), "Purple: Current scene in the game."));
		else if (marker == SceneSettingMarker::Overwrite)
			FeatureListRenderer::SetSettingHint(T(TKEY("scene_picker_preset_hint"), "Orange: Contains settings from an enabled preset."));
		else if (marker == SceneSettingMarker::Settings)
			FeatureListRenderer::SetSettingHint(T(TKEY("scene_picker_user_hint"), "Green: Contains your saved scene settings."));
	}

	bool BeginScenePickerCombo(const char* id, const char* label, bool current, SceneSettingMarker marker)
	{
		Util::ComboPreviewAction preview{ .previewColor = ImGui::GetColorU32(GetScenePickerTextColor(current, marker)) };
		const bool open = Util::BeginComboWithAction(id, label, preview);
		if (!open)
			ReportScenePickerHint(current, marker);
		return open;
	}

	bool DrawWeatherPicker(RE::FormID& selectedWeather, std::string_view feature = {}, float* scroll = nullptr);
	bool DrawLocationPicker(std::optional<SceneSettingsManager::LocationTarget>& selectedLocation, std::string_view feature = {}, float* scroll = nullptr);

	float ComboWidth(float textWidth)
	{
		return std::ceil(textWidth + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
	}

	float GetPeriodPickerWidth(SceneSettingsManager::SceneContextId context)
	{
		using Type = SceneSettingsManager::SceneContextType;
		if (context.type == Type::Interior)
			return 0.0f;
		float width = 0.0f;
		const auto measure = [&](const char* label) {
			width = std::max(width, ImGui::CalcTextSize(label).x);
		};
		if (context.type != Type::TimeOfDay)
			measure(T("feature.scene_manager.weather.normal", "Normal"));
		for (int period = 0; period < kPeriodCount; ++period)
			measure(GetPeriodLabel(period));
		return ComboWidth(width);
	}

	void DrawFeatureTargetPicker(const std::string& name, SceneSettingsManager::SceneContextId context, float pickerWidth)
	{
		using Type = SceneSettingsManager::SceneContextType;
		const bool periodicTarget = context.type == Type::Weather || context.type == Type::Location;
		const float available = ImGui::GetContentRegionAvail().x;
		const float periodWidth = std::min(available, pickerWidth);
		const bool inlinePeriod = available >= periodWidth + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
		if (periodicTarget) {
			ImGui::SetNextItemWidth(inlinePeriod ? available - periodWidth - ImGui::GetStyle().ItemSpacing.x : available);
			if (context.type == Type::Weather) {
				auto weather = context.weatherId;
				if (DrawWeatherPicker(weather, name)) {
					context.weatherId = weather;
					context.period = SceneSettingsManager::GetSingleton()->IsSceneTimeOfDayEnabled(context) ? SceneSettingsManager::GetCurrentPeriod() : TimeOfDayPeriod::Count;
					RequestFeatureContext(name, context);
				}
			} else {
				const auto& targets = SceneSettingsManager::GetSingleton()->GetLocationCatalog();
				const auto found = std::ranges::find_if(targets, [&](const auto& target) {
					return target.type == context.locationType && target.formKey == context.locationFormKey;
				});
				std::optional<SceneSettingsManager::LocationTarget> target;
				if (found != targets.end())
					target = *found;
				if (DrawLocationPicker(target, name) && target) {
					context.locationType = target->type;
					context.locationFormKey = target->formKey;
					context.period = SceneSettingsManager::GetSingleton()->IsSceneTimeOfDayEnabled(context) ? SceneSettingsManager::GetCurrentPeriod() : TimeOfDayPeriod::Count;
					RequestFeatureContext(name, context);
				}
			}
			if (inlinePeriod)
				ImGui::SameLine();
		}
		ImGui::SetNextItemWidth(periodWidth);
		const char* preview = context.period == TimeOfDayPeriod::Count ?
		                          T("feature.scene_manager.weather.normal", "Normal") :
		                          GetPeriodLabel(static_cast<int>(context.period));
		const auto currentPeriod = SceneSettingsManager::GetCurrentPeriod();
		if (BeginScenePickerCombo("##ScenePeriod", preview, context.period == currentPeriod, GetFeatureSceneMarker(name, context))) {
			const auto drawPeriod = [&](TimeOfDayPeriod period, const char* label) {
				auto candidate = context;
				candidate.period = period;
				const auto marker = GetFeatureSceneMarker(name, candidate);
				ImGui::PushID(static_cast<int>(period));
				ImGui::PushStyleColor(ImGuiCol_Text, GetScenePickerTextColor(period == currentPeriod, marker));
				if (Util::RoundedSelectable(label, context.period == period))
					RequestFeatureContext(name, candidate);
				ReportScenePickerHint(period == currentPeriod, marker);
				if (context.period == period)
					ImGui::SetItemDefaultFocus();
				ImGui::PopStyleColor();
				ImGui::PopID();
			};
			if (periodicTarget)
				drawPeriod(TimeOfDayPeriod::Count, T("feature.scene_manager.weather.normal", "Normal"));
			for (int period = 0; period < kPeriodCount; ++period)
				drawPeriod(static_cast<TimeOfDayPeriod>(period), GetPeriodLabel(period));
			ImGui::EndCombo();
		}
	}

	std::optional<std::string> GetLocationTravelCommand(const SceneSettingsManager::SceneContextId& context)
	{
		const auto& targets = SceneSettingsManager::GetSingleton()->GetLocationCatalog();
		const auto target = std::ranges::find_if(targets, [&](const auto& candidate) {
			return candidate.type == context.locationType && candidate.formKey == context.locationFormKey;
		});
		return target == targets.end() ? std::nullopt :
		                                 Util::SceneTravel::GetCommand(RE::TESForm::LookupByID(target->formId), target->cocCode);
	}

	bool CanPreviewContext(const SceneSettingsManager::SceneContextId& context)
	{
		if (!globals::game::player || !globals::game::player->parentCell || !globals::state ||
			globals::state->IsMainOrLoadingMenuOpen(globals::game::ui) || context.type == SceneSettingsManager::SceneContextType::Interior)
			return false;
		if (context.period != TimeOfDayPeriod::Count &&
			(!globals::game::calendar || !globals::game::calendar->gameHour || !globals::game::calendar->timeScale))
			return false;
		if (context.type == SceneSettingsManager::SceneContextType::Weather)
			return globals::game::sky && RE::TESForm::LookupByID<RE::TESWeather>(context.weatherId);
		return context.type != SceneSettingsManager::SceneContextType::Location || GetLocationTravelCommand(context).has_value();
	}

	std::optional<SceneSettingsManager::SceneContextId> GetStandaloneContext()
	{
		using Type = SceneSettingsManager::SceneContextType;
		switch (standalonePage) {
		case 0:
			return SceneSettingsManager::SceneContextId{ .type = Type::Interior };
		case 1:
			return SceneSettingsManager::SceneContextId{ .type = Type::TimeOfDay, .period = SceneSettingsManager::GetCurrentPeriod() };
		case 2:
			if (standaloneLocation)
				return SceneSettingsManager::SceneContextId{ .type = Type::Location,
					.locationType = standaloneLocation->type,
					.locationFormKey = standaloneLocation->formKey };
			break;
		case 3:
			if (standaloneWeather)
				return SceneSettingsManager::SceneContextId{ .type = Type::Weather, .weatherId = standaloneWeather };
			break;
		}
		return std::nullopt;
	}

	void DrawLayerManagement(const std::optional<SceneSettingsManager::SceneContextId>& target = std::nullopt)
	{
		using Manager = SceneSettingsManager;
		using Type = Manager::SceneContextType;
		using Source = Manager::EntrySource;
		auto* manager = Manager::GetSingleton();
		if (manager->HasPendingContextEdit())
			Util::Text::WrappedWarning("%s", SceneSettingsUI::PendingContextEditMessage());
		const auto& style = ImGui::GetStyle();
		const auto* pauseLabel = T("feature.scene_manager.action.pause_all", "Pause All");
		const auto* resumeLabel = T("feature.scene_manager.action.unpause_all", "Unpause All");
		const auto* deleteLabel = T("feature.scene_manager.action.delete_all", "Delete All");
		const float deleteWidth = ImGui::CalcTextSize(deleteLabel).x + 2.0f * style.FramePadding.x;
		const float gap = style.ItemInnerSpacing.x;
		const auto drawActions = [&](Source source, const Manager::EntryLayerScope& scope, const char* heading = nullptr) {
			ImGui::PushID(static_cast<int>(source));
			const auto summary = manager->GetEntryLayerSummary(source, scope);
			const auto* pauseAction = summary.AllPaused() ? resumeLabel : pauseLabel;
			const float actionsWidth = ImGui::CalcTextSize(pauseAction).x + 2.0f * style.FramePadding.x + gap + deleteWidth;
			const auto start = ImGui::GetCursorPosX();
			const auto available = ImGui::GetContentRegionAvail().x;
			const auto count = heading ? std::string{} : std::to_string(summary.total);
			const auto* text = heading ? heading : count.c_str();
			const float groupWidth = ImGui::CalcTextSize(text).x + style.ItemSpacing.x + actionsWidth;
			ImGui::SetCursorPosX(start + std::max(0.0f, available - groupWidth));
			ImGui::TextUnformatted(text);
			BrowserUI::SameLineIfFits(actionsWidth);
			const Util::DisableGuard disabled(manager->HasPendingContextEdit() || summary.total == 0);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));
			const bool toggle = ImGui::Button(pauseAction);
			ImGui::SameLine(0.0f, gap);
			const bool remove = ImGui::Button(deleteLabel);
			ImGui::PopStyleVar();
			if (toggle)
				globals::menu->GetUndoHistory().Change(SceneSettingsUI::MakeUndoTarget(), [&] {
					manager->SetEntryLayerPaused(source, !summary.AllPaused(), scope);
				});
			if (remove) {
				pendingDeleteScope = scope;
				pendingDeleteSource = source;
				deleteSceneChangesConfirmation.title = T(TKEY("scene_page_delete_changes_title"), "Delete changes");
				deleteSceneChangesConfirmation.message = source == Source::User ?
				                                             T(TKEY("scene_manage_delete_user"), "Delete your settings in this scene group? Installed settings remain. Undo restores your changes.") :
				                                             T(TKEY("scene_manage_delete_presets"), "Delete the loaded preset scene files in this group? This removes files from disk.");
				deleteSceneChangesConfirmation.confirmLabel = T("feature.scene_manager.action.delete", "Delete");
				deleteSceneChangesConfirmation.cancelLabel = T(TKEY("cancel"), "Cancel");
				deleteSceneChangesConfirmation.Request();
			}
			ImGui::PopID();
		};
		const auto drawGroup = [&](const char* label, const Manager::EntryLayerScope& scope) {
			ImGui::PushID(label);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label);
			ImGui::TableNextColumn();
			drawActions(Source::User, scope);
			ImGui::TableNextColumn();
			drawActions(Source::Overwrite, scope);
			ImGui::PopID();
		};
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::max(style.CellPadding.x, style.ItemSpacing.x), style.CellPadding.y));
		if (ImGui::BeginTable("##SceneLayerActions", target ? 2 : 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoSavedSettings)) {
			const char* labels[] = { T("feature.scene_manager.section.user_settings", "User Settings"),
				T("feature.scene_manager.section.overwrite_files", "Preset Settings") };
			const std::array types{ Type::Interior, Type::TimeOfDay, Type::Location, Type::Weather };
			const char* groupLabels[] = { T("feature.scene_manager.tab.interior", "Interior"), T("feature.scene_manager.tab.time_of_day", "Time of Day"),
				T("feature.scene_manager.tab.locations", "Locations"), T("feature.scene_manager.tab.weather", "Weather") };
			const auto* scopeLabel = T(TKEY("scene_manage_scope"), "Scope");
			if (!target) {
				float width = ImGui::CalcTextSize(scopeLabel).x;
				for (const auto* label : groupLabels)
					width = std::max(width, ImGui::CalcTextSize(label).x);
				ImGui::TableSetupColumn(scopeLabel, ImGuiTableColumnFlags_WidthFixed, width);
			}
			for (const auto* label : labels)
				ImGui::TableSetupColumn(label);
			if (target) {
				ImGui::TableHeadersRow();
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const Manager::EntryLayerScope scope{ .type = target->type, .target = target };
				drawActions(Source::User, scope);
				ImGui::TableNextColumn();
				drawActions(Source::Overwrite, scope);
			} else {
				ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(scopeLabel);
				for (int column = 0; column < IM_ARRAYSIZE(labels); ++column) {
					ImGui::TableNextColumn();
					drawActions(column == 0 ? Source::User : Source::Overwrite, {}, labels[column]);
				}
				for (size_t index = 0; index < types.size(); ++index)
					drawGroup(groupLabels[index], { .type = types[index] });
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		Util::Text::WrappedDisabled("%s", T(TKEY("scene_manage_pause_session"), "Pausing installed settings lasts until they are reloaded."));
	}

	void DrawLayerDeleteConfirmation()
	{
		if (deleteSceneChangesConfirmation.Draw()) {
			auto* manager = SceneSettingsManager::GetSingleton();
			globals::menu->GetUndoHistory().Change(SceneSettingsUI::MakeUndoTarget(), [manager] {
				const auto result = manager->DeleteEntryLayer(pendingDeleteSource, pendingDeleteScope);
				if (result.failed != 0)
					EditorWindow::GetSingleton()->ShowNotification(T(TKEY("scene_manage_delete_failed"), "Some scene files could not be deleted."), Util::Colors::GetError());
			});
		}
	}

	void DrawManageSettings()
	{
		const auto title = std::format("{}###SceneManagerSettings", T("feature.scene_manager.action.manage_settings", "Manage Scene Settings"));
		manageSettingsModal.Draw(title.c_str(), ThemeManager::Constants::Em(manageSection == 0 ? kManageModalWidthEm : kCopyModalWidthEm), [] {
			if (EditorWindow::ClosePopupOnEscape())
				return;
			const char* sections[] = { T(TKEY("scene_manage_layers"), "Manage scenes"),
				T("feature.scene_manager.copy.title", "Copy Scene Settings") };
			Util::SegmentedControl("##SceneManagementSections", sections, IM_ARRAYSIZE(sections), manageSection);
			ImGui::Spacing();
			switch (manageSection) {
			case 0:
				DrawLayerManagement();
				break;
			case 1:
				SceneCopyModal::DrawEmbedded();
				break;
			}
			DrawLayerDeleteConfirmation();
		});
	}
}

UndoHistory::Target SceneSettingsUI::MakeUndoTarget()
{
	using Snapshot = SceneSettingsManager::UserSettingsSnapshot;
	UndoHistory::Target target{ .key = "scene", .label = T(TKEY("scene_undo_label"), "Scene Manager") };
	target.capture = []() -> UndoHistory::SnapshotPtr {
		auto* manager = SceneSettingsManager::GetSingleton();
		return manager ? UndoHistory::Capture(manager->CaptureUserSettings()) : nullptr;
	};
	target.restore = [](const UndoHistory::Snapshot& snapshot) {
		if (auto* manager = SceneSettingsManager::GetSingleton())
			manager->RestoreUserSettings(UndoHistory::Get<Snapshot>(snapshot));
	};
	return target;
}

/** @brief Registers scene edits with the active menu or editor history. */
static void WatchSceneSettingsForUndo()
{
	globals::menu->WatchUndoTarget(SceneSettingsUI::MakeUndoTarget());
}

void SceneSettingsUI::OpenSceneContext(const SceneSettingsManager::SceneContextId& context,
	const std::string& featureShortName)
{
	if (!featureShortName.empty()) {
		RequestFeatureContext(featureShortName, context);
		globals::menu->OpenFeaturePage(featureShortName);
		return;
	}
	standalonePage = context.type == SceneSettingsManager::SceneContextType::Location  ? 2 :
	                 context.type == SceneSettingsManager::SceneContextType::Weather   ? 3 :
	                 context.type == SceneSettingsManager::SceneContextType::TimeOfDay ? 1 :
	                                                                                     0;
	standaloneWeather = context.weatherId;
	if (context.type == SceneSettingsManager::SceneContextType::Location) {
		const auto& catalog = SceneSettingsManager::GetSingleton()->GetLocationCatalog();
		const auto target = std::ranges::find_if(catalog, [&](const auto& candidate) {
			return candidate.type == context.locationType && candidate.formKey == context.locationFormKey;
		});
		if (target != catalog.end())
			standaloneLocation = *target;
	}
	globals::menu->OpenSceneManager();
}

bool SceneSettingsUI::LayerListsFeature(SceneSettingsManager::SceneContextType layer, const std::string& featureShortName)
{
	const SceneSettingsManager::SceneContextId context{ .type = layer };
	if (!SceneSettingsManager::IsFeatureAllowedForType(SceneSettingsContextRules::GetSceneContextRules(context).sceneType, featureShortName))
		return false;
	const bool transitionableOnly = layer == SceneSettingsManager::SceneContextType::Weather;
	return std::ranges::any_of(GetSceneFeatures(transitionableOnly),
		[&](Feature* feature) { return feature->GetShortName() == featureShortName; });
}

std::optional<SceneSettingsManager::SceneContextId> SceneSettingsUI::ResolveCurrentLayer(
	SceneSettingsManager::SceneContextType layer, const std::string& featureShortName)
{
	return LayerListsFeature(layer, featureShortName) ? ResolveCurrentContext(layer) : std::nullopt;
}

std::optional<SceneSettingsManager::SceneContextId> SceneSettingsUI::ResolveCurrentContext(SceneSettingsManager::SceneContextType layer)
{
	using enum SceneSettingsManager::SceneContextType;
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return std::nullopt;

	switch (layer) {
	case TimeOfDay:
		return SceneSettingsManager::SceneContextId{ .type = TimeOfDay, .period = SceneSettingsManager::GetCurrentPeriod() };
	case Interior:
		return SceneSettingsManager::SceneContextId{ .type = Interior };
	case Weather:
		{
			auto* sky = globals::game::sky;
			if (!sky || !sky->currentWeather)
				return std::nullopt;
			return SceneSettingsManager::SceneContextId{ .type = Weather, .weatherId = sky->currentWeather->GetFormID() };
		}
	default:
		{
			// Outermost first, so the last listed link is the narrowest: the one whose settings win here.
			const auto& chain = manager->GetCurrentLocationTargets();
			const auto narrowest = std::ranges::find_if(chain | std::views::reverse, [&](const auto& target) {
				return manager->IsLocationTargetAuthored(target.type, target.formKey);
			});
			if (narrowest == (chain | std::views::reverse).end())
				return std::nullopt;
			return SceneSettingsManager::SceneContextId{
				.type = Location,
				.locationType = narrowest->type,
				.locationFormKey = narrowest->formKey,
			};
		}
	}
}

void SceneSettingsUI::OpenLocationsPage()
{
	standalonePage = 2;
	globals::menu->OpenSceneManager();
}

void SceneSettingsUI::OpenSceneManagerPage()
{
	standalonePage = Util::IsInterior() ? 0 : 1;
	globals::menu->OpenSceneManager();
}

bool SceneSettingsUI::OpenCurrentLocationForSetting(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* editorWindow = EditorWindow::GetSingleton();
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!editorWindow || !manager)
		return false;

	const auto warn = [editorWindow](const char* message) {
		editorWindow->ShowNotification(message, Util::Colors::GetWarning(), EditorWindow::kLongNotificationDuration);
	};

	const auto& targets = manager->GetCurrentLocationTargets();
	if (targets.empty()) {
		warn(T(TKEY("scene_override_location_unknown"),
			"Can't tell where you are yet. Step into the world, then try again."));
		return false;
	}

	// Flat Location is the layer that accepts non-blendable settings Weather/TOD grey out.
	if (!SceneSettingsManager::IsSettingAllowedForType(SceneSettingsManager::SceneType::Location,
			featureShortName, settingPath, settingKey, false)) {
		warn(T(TKEY("scene_override_location_unsupported"),
			"This place can't hold that setting: Location scenes don't support it either."));
		return false;
	}

	// Chain is outermost-first; the last link is the place you're actually standing in.
	const auto& target = targets.back();
	const SceneSettingsManager::SceneContextId locationContext{
		.type = SceneSettingsManager::SceneContextType::Location,
		.period = TimeOfDayPeriod::Count,
		.locationType = target.type,
		.locationFormKey = target.formKey,
	};
	OpenSceneContext(locationContext, featureShortName);
	return true;
}

namespace
{
	constexpr int kSceneTargetComboVisibleItems = 12;
	float weatherPickerScroll = 0.0f;
	float locationPickerScroll = 0.0f;

	struct ScenePickerPresentation
	{
		std::string label;
		bool configured = false;
		SceneSettingMarker marker = SceneSettingMarker::None;
	};

	const ScenePickerPresentation& GetScenePickerPresentation(const SceneSettingsManager::SceneContextId& context,
		std::string label)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		static std::uint64_t cachedRevision = std::numeric_limits<std::uint64_t>::max();
		static std::string cachedLocale;
		static std::map<SceneSettingsManager::SceneContextId, ScenePickerPresentation> labels;
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		if (cachedRevision != manager->GetEntryPresentationRevision() || cachedLocale != locale) {
			cachedRevision = manager->GetEntryPresentationRevision();
			cachedLocale = locale;
			labels.clear();
		}
		auto [it, inserted] = labels.try_emplace(context);
		if (inserted) {
			if (manager->IsSceneTimeOfDayEnabled(context))
				label = std::format("{} ({})", label, T("feature.scene_manager.mode.time_of_day_short", "TOD"));
			const auto summary = manager->GetFeatureSceneSummary({}, context, false);
			it->second = { std::move(label), summary.total != 0,
				summary.presetSettings ? SceneSettingMarker::Overwrite :
				summary.total          ? SceneSettingMarker::Settings :
										 SceneSettingMarker::None };
		}
		return it->second;
	}

	bool BeginSceneTargetCombo(const char* id, const char* preview, bool current, SceneSettingMarker marker,
		bool showStatus, float* scroll, Util::ComboPreviewAction& action)
	{
		const auto& style = ImGui::GetStyle();
		const float width = std::min(ImGui::GetMainViewport()->WorkSize.x - style.WindowPadding.x * 2.0f,
			std::max(ImGui::CalcItemWidth(), ThemeManager::Constants::Em(kSceneTargetPopupWidthEm)));
		const float height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * kSceneTargetComboVisibleItems + style.WindowPadding.y * 2.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, height));
		if (showStatus)
			action.previewColor = ImGui::GetColorU32(GetScenePickerTextColor(current, marker));
		const bool open = Util::BeginSearchableCombo(id, preview, ImGuiComboFlags_None, nullptr, kSceneTargetComboVisibleItems, scroll, &action);
		if (showStatus && !open)
			ReportScenePickerHint(current, marker);
		return open;
	}

	void RequestTargetManagement(const SceneSettingsManager::SceneContextId& target, const std::string& label)
	{
		SceneEntryTable::EndPresetEditing();
		managedTarget = { target, label };
		targetManagementRequested = true;
	}

	void DrawTargetManagement()
	{
		const auto title = std::format("{}###ManageSceneTarget", managedTarget.second);
		if (std::exchange(targetManagementRequested, false)) {
			ImGui::OpenPopup(title.c_str());
			ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
		}
		if (!ImGui::IsPopupOpen(title.c_str()))
			return;
		const float titleWidth = ImGui::CalcTextSize(managedTarget.second.c_str()).x + 2.0f * ImGui::GetFrameHeightWithSpacing();
		const auto& style = ImGui::GetStyle();
		const auto* viewport = ImGui::GetMainViewport();
		const float width = std::min(std::max(ThemeManager::Constants::Em(kTargetManageWidthEm), titleWidth),
			viewport->WorkSize.x - style.WindowPadding.x * 2.0f);
		ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, viewport->WorkSize.y - style.WindowPadding.y * 2.0f));
		if (auto popup = Util::Popup(title.c_str(), title.c_str())) {
			if (EditorWindow::ClosePopupOnEscape())
				return;
			DrawLayerManagement(managedTarget.first);
			DrawLayerDeleteConfirmation();
		}
	}

	bool DrawSceneTargetSelectable(const char* id, const ScenePickerPresentation& presentation, bool selected, bool current,
		const SceneSettingsManager::SceneContextId& context, std::string_view feature, bool featureOnly)
	{
		ImGui::PushID(id);
		const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
		const bool canManage = feature.empty() && presentation.configured;
		const auto start = ImGui::GetCursorScreenPos();
		const float right = start.x + ImGui::GetContentRegionAvail().x;
		const char* manageLabel = context.type == SceneSettingsManager::SceneContextType::Weather ?
		                              T("feature.scene_manager.action.manage_weather", "Manage Weather") :
		                              T("feature.scene_manager.action.manage_location", "Manage Location");
		const float buttonWidth = ImGui::CalcTextSize(manageLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		const float buttonX = std::max(start.x, right - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x);
		const bool manageHovered = canManage && ImGui::IsWindowHovered() &&
		                           ImGui::IsMouseHoveringRect(ImVec2(buttonX, start.y), ImVec2(right, start.y + ImGui::GetTextLineHeight()));
		const bool pressed = Util::RoundedSelectable("##SceneTarget", selected || manageHovered,
			ImGuiSelectableFlags_NoAutoClosePopups | (canManage ? ImGuiSelectableFlags_AllowOverlap : ImGuiSelectableFlags_None));
		if (selected)
			ImGui::SetItemDefaultFocus();
		auto color = ImGui::GetColorU32(ImGuiCol_Text);
		if (!feature.empty()) {
			const auto marker = featureOnly ? GetFeatureSceneMarker(feature, context, false) : presentation.marker;
			ReportScenePickerHint(current, marker);
			color = ImGui::GetColorU32(GetScenePickerTextColor(current, marker));
		}
		const auto& label = presentation.label;
		const ImVec4 clip(start.x, start.y, canManage ? buttonX - ImGui::GetStyle().ItemSpacing.x : right,
			start.y + ImGui::GetTextLineHeight());
		ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), start,
			color, label.c_str(), nullptr, 0.0f, &clip);
		if (ImGui::CalcTextSize(label.c_str()).x > clip.z - clip.x)
			Util::AddTooltip(label.c_str());
		if (canManage) {
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(buttonX, start.y));
			if (ImGui::SmallButton(manageLabel)) {
				RequestTargetManagement(context, presentation.label);
				ImGui::CloseCurrentPopup();
				return false;
			}
		}
		if (pressed && !manageHovered)
			ImGui::CloseCurrentPopup();
		return pressed && !manageHovered;
	}

	std::string GetWeatherTargetLabel(const RE::TESWeather* weather)
	{
		if (!weather)
			return {};
		auto label = Util::GetFormDisplayName(weather->GetFormID());
		return label.empty() ? Util::FormIdToSpid(weather->GetFormID()) : label;
	}

	const std::vector<RE::TESWeather*>& GetSceneWeatherTargets()
	{
		static RE::TESDataHandler* cachedDataHandler = nullptr;
		static size_t cachedWeatherCount = std::numeric_limits<size_t>::max();
		static std::string cachedLocale;
		static std::vector<RE::TESWeather*> targets;
		static std::set<RE::FormID> targetIds;
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		const auto weatherCount = dataHandler ? dataHandler->GetFormArray<RE::TESWeather>().size() : 0;
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		bool targetsChanged = dataHandler != cachedDataHandler || weatherCount != cachedWeatherCount || cachedLocale != locale;
		if (targetsChanged) {
			cachedDataHandler = dataHandler;
			cachedWeatherCount = weatherCount;
			cachedLocale = locale;
			targets.clear();
			targetIds.clear();
			if (dataHandler)
				for (auto* weather : dataHandler->GetFormArray<RE::TESWeather>())
					if (weather && weather->GetFormID() != 0 && targetIds.insert(weather->GetFormID()).second)
						targets.push_back(weather);
		}
		if (auto* sky = globals::game::sky)
			for (auto* weather : { sky->currentWeather, sky->lastWeather })
				if (weather && weather->GetFormID() != 0 && targetIds.insert(weather->GetFormID()).second) {
					targets.push_back(weather);
					targetsChanged = true;
				}
		if (targetsChanged)
			std::ranges::sort(targets, [](const auto* lhs, const auto* rhs) {
				return std::tuple{ GetWeatherTargetLabel(lhs), lhs->GetFormID() } <
				       std::tuple{ GetWeatherTargetLabel(rhs), rhs->GetFormID() };
			});
		return targets;
	}

	bool DrawWeatherPicker(RE::FormID& selectedWeather, std::string_view feature, float* scroll)
	{
		using Context = SceneSettingsManager::SceneContextId;
		using Type = SceneSettingsManager::SceneContextType;
		bool changed = false;
		static std::map<std::string, bool> filters;
		auto& filter = filters[std::string(feature)];
		const auto& targets = GetSceneWeatherTargets();
		auto findWeather = [&](RE::FormID id) {
			return std::ranges::find_if(targets, [id](const auto* weather) { return weather->GetFormID() == id; });
		};
		if (findWeather(selectedWeather) == targets.end()) {
			selectedWeather = 0;
			if (auto* sky = globals::game::sky; sky && sky->currentWeather &&
												findWeather(sky->currentWeather->GetFormID()) != targets.end())
				selectedWeather = sky->currentWeather->GetFormID();
		}
		const auto selected = findWeather(selectedWeather);
		const auto preview = selected != targets.end() ?
		                         GetScenePickerPresentation({ .type = Type::Weather, .weatherId = selectedWeather }, GetWeatherTargetLabel(*selected)).label :
		                         std::string(T("feature.scene_manager.weather.select_target", "Select a weather..."));
		const Context previewContext{ .type = Type::Weather, .weatherId = selectedWeather };
		const bool canManage = feature.empty() && selected != targets.end() &&
		                       GetScenePickerPresentation(previewContext, GetWeatherTargetLabel(*selected)).configured;
		Util::ComboPreviewAction action{ .label = canManage ? T("feature.scene_manager.action.manage_weather", "Manage Weather") : nullptr };
		const SKSE::stl::scope_exit drawPreviewManagement([&] {
			if (action.pressed)
				RequestTargetManagement(previewContext, preview);
		});

		const auto isCurrent = [](const RE::TESWeather* weather) {
			const auto* sky = globals::game::sky;
			return sky && (sky->currentWeather == weather || sky->lastWeather == weather);
		};
		const auto marker = selected == targets.end() || feature.empty() ? SceneSettingMarker::None :
		                    filter                                       ? GetFeatureSceneMarker(feature, previewContext, false) :
		                                                                   GetScenePickerPresentation(previewContext, GetWeatherTargetLabel(*selected)).marker;
		if (!BeginSceneTargetCombo(feature.empty() ? T("feature.scene_manager.weather.target", "Weather") : "##FeatureSceneWeather",
				preview.c_str(), selected != targets.end() && isCurrent(*selected), marker, !feature.empty(), scroll ? scroll : &weatherPickerScroll, action))
			return false;
		std::vector<RE::TESWeather*> current;
		std::vector<RE::TESWeather*> configured;
		std::vector<RE::TESWeather*> available;
		for (auto* weather : targets) {
			const auto& presentation = GetScenePickerPresentation({ .type = Type::Weather, .weatherId = weather->GetFormID() },
				GetWeatherTargetLabel(weather));
			(presentation.configured ? configured : isCurrent(weather) ? current :
																		 available)
				.push_back(weather);
		}
		const auto drawGroup = [&](const char* label, const std::vector<RE::TESWeather*>& candidates) {
			std::vector<RE::TESWeather*> visible;
			int selectedIndex = -1;
			for (auto* weather : candidates) {
				const auto& presentation = GetScenePickerPresentation({ .type = Type::Weather, .weatherId = weather->GetFormID() },
					GetWeatherTargetLabel(weather));
				if (!Util::SearchableComboMatches(presentation.label) || (!feature.empty() && filter &&
																			 GetFeatureSceneMarker(feature, { .type = Type::Weather, .weatherId = weather->GetFormID() }, false) == SceneSettingMarker::None))
					continue;
				if (weather->GetFormID() == selectedWeather)
					selectedIndex = static_cast<int>(visible.size());
				visible.push_back(weather);
			}
			if (visible.empty())
				return;
			ImGui::SeparatorText(label);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visible.size()));
			if (selectedIndex >= 0)
				clipper.IncludeItemByIndex(selectedIndex);
			while (clipper.Step())
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
					auto* weather = visible[index];
					const Context context{ .type = Type::Weather, .weatherId = weather->GetFormID() };
					const auto& presentation = GetScenePickerPresentation(context, GetWeatherTargetLabel(weather));
					const auto id = std::format("CopyWeather{:08X}", weather->GetFormID());
					if (DrawSceneTargetSelectable(id.c_str(), presentation, weather->GetFormID() == selectedWeather, isCurrent(weather), context, feature, filter)) {
						selectedWeather = weather->GetFormID();
						changed = true;
					}
				}
		};
		if (!feature.empty())
			DrawFeatureFilter(feature, filter);
		drawGroup(T("feature.scene_manager.weather.group.current", "Current"), current);
		drawGroup(T("feature.scene_manager.weather.group.configured", "Configured"), configured);
		drawGroup(T("feature.scene_manager.weather.group.available", "Available"), available);
		Util::EndSearchableCombo();
		return changed;
	}

	std::string GetLocationTargetLabel(const SceneSettingsManager::LocationTarget& target)
	{
		const auto* typeName = GetLocationTypeLabel(target.type);
		return std::vformat(T("feature.scene_manager.location.target_label", "{0} ({1})"),
			std::make_format_args(target.name.empty() ? target.formKey : target.name, typeName));
	}

	bool DrawLocationPicker(std::optional<SceneSettingsManager::LocationTarget>& selectedLocation, std::string_view feature, float* scroll)
	{
		using Target = SceneSettingsManager::LocationTarget;
		using Type = SceneSettingsManager::LocationTargetType;
		using ContextType = SceneSettingsManager::SceneContextType;
		bool changed = false;
		static std::map<std::string, bool> filters;
		auto& filter = filters[std::string(feature)];
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& targets = manager->GetLocationCatalog();
		const auto& current = manager->GetCurrentLocationTargets();
		const auto isCurrent = [&](const Target& target) {
			return std::ranges::any_of(current, [&](const auto& candidate) { return IsSameLocationTarget(candidate, target); });
		};
		const auto getPresentation = [&](const Target& target) -> const ScenePickerPresentation& {
			return GetScenePickerPresentation({ .type = ContextType::Location, .locationType = target.type, .locationFormKey = target.formKey }, GetLocationTargetLabel(target));
		};
		if (!selectedLocation) {
			for (auto candidate = current.rbegin(); candidate != current.rend(); ++candidate) {
				const auto found = std::ranges::find_if(targets, [&](const auto& target) { return IsSameLocationTarget(target, *candidate); });
				if (found != targets.end()) {
					selectedLocation = *found;
					break;
				}
			}
			if (!selectedLocation) {
				const auto configured = std::ranges::find_if(targets, [&](const auto& target) { return getPresentation(target).configured; });
				if (configured != targets.end())
					selectedLocation = *configured;
			}
		}
		const auto preview = selectedLocation ? getPresentation(*selectedLocation).label :
		                                        std::string(T("feature.scene_manager.location.select_target", "Select a worldspace, region, location type, location, or cell..."));
		const SceneSettingsManager::SceneContextId previewContext{ .type = ContextType::Location,
			.locationType = selectedLocation ? selectedLocation->type : Type::Worldspace,
			.locationFormKey = selectedLocation ? selectedLocation->formKey : std::string{} };
		const bool canManage = feature.empty() && selectedLocation && getPresentation(*selectedLocation).configured;
		Util::ComboPreviewAction action{ .label = canManage ? T("feature.scene_manager.action.manage_location", "Manage Location") : nullptr };
		const SKSE::stl::scope_exit drawPreviewManagement([&] {
			if (action.pressed)
				RequestTargetManagement(previewContext, preview);
		});

		const auto marker = !selectedLocation || feature.empty() ? SceneSettingMarker::None :
		                    filter                               ? GetFeatureSceneMarker(feature, previewContext, false) :
		                                                           getPresentation(*selectedLocation).marker;
		if (!BeginSceneTargetCombo(feature.empty() ? T("feature.scene_manager.location.target", "Target") : "##FeatureSceneLocation",
				preview.c_str(), selectedLocation && isCurrent(*selectedLocation), marker, !feature.empty(), scroll ? scroll : &locationPickerScroll, action))
			return false;
		std::vector<const Target*> currentTargets;
		std::vector<const Target*> configured;
		std::vector<const Target*> locationTypes;
		std::vector<const Target*> available;
		for (const auto& target : targets) {
			const auto& presentation = getPresentation(target);
			(target.type == Type::LocationType ? locationTypes : presentation.configured ? configured :
															 isCurrent(target)           ? currentTargets :
																						   available)
				.push_back(&target);
		}
		std::ranges::sort(currentTargets, [&](const auto* lhs, const auto* rhs) {
			const auto left = std::ranges::find_if(current, [&](const auto& target) { return IsSameLocationTarget(target, *lhs); });
			const auto right = std::ranges::find_if(current, [&](const auto& target) { return IsSameLocationTarget(target, *rhs); });
			return left > right;
		});
		const auto drawGroup = [&](const char* label, const std::vector<const Target*>& candidates, bool types = false) {
			std::vector<const Target*> visible;
			int selectedIndex = -1;
			for (const auto* target : candidates) {
				if (!Util::SearchableComboMatches(getPresentation(*target).label) || (!feature.empty() && filter &&
																						 GetFeatureSceneMarker(feature, { .type = ContextType::Location, .locationType = target->type, .locationFormKey = target->formKey }, false) == SceneSettingMarker::None))
					continue;
				if (selectedLocation && IsSameLocationTarget(*selectedLocation, *target))
					selectedIndex = static_cast<int>(visible.size());
				visible.push_back(target);
			}
			if (visible.empty())
				return;
			if (types && Util::GetSearchableComboFilter().empty()) {
				ImGui::SeparatorText("");
				if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoTreePushOnOpen))
					return;
			} else {
				ImGui::SeparatorText(label);
			}
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visible.size()));
			if (selectedIndex >= 0)
				clipper.IncludeItemByIndex(selectedIndex);
			while (clipper.Step())
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
					const auto& target = *visible[index];
					const auto id = std::format("{}:{}", static_cast<int>(target.type), target.formKey);
					if (DrawSceneTargetSelectable(id.c_str(), getPresentation(target),
							selectedLocation && IsSameLocationTarget(*selectedLocation, target), isCurrent(target),
							{ .type = ContextType::Location, .locationType = target.type, .locationFormKey = target.formKey }, feature, filter)) {
						selectedLocation = target;
						changed = true;
					}
				}
		};
		if (!feature.empty())
			DrawFeatureFilter(feature, filter);
		drawGroup(T("feature.scene_manager.location.group.current", "Current"), currentTargets);
		drawGroup(T("feature.scene_manager.location.group.configured", "Configured"), configured);
		drawGroup(T("feature.scene_manager.location.group.types", "Location Types"), locationTypes, true);
		drawGroup(T("feature.scene_manager.location.group.available", "Available"), available);
		Util::EndSearchableCombo();
		return changed;
	}
}

void SceneSettingsUI::DrawStandalonePage()
{
	WatchSceneSettingsForUndo();
	if (Util::Button(T("feature.scene_manager.action.manage_settings", "Manage Scene Settings"))) {
		SceneEntryTable::EndPresetEditing();
		SceneCopyModal::PrepareEmbedded();
		manageSettingsModal.Request();
	}
	BrowserUI::SameLineIfFits(ImGui::GetFrameHeight());
	{
		const Util::DisableGuard disabled(!PresetExportPanel::CanExport());
		const auto style = Util::TransparentIconButtonStyle();
		const float size = ImGui::GetFrameHeight();
		if (Icons::Button("##ExportPreset", SceneActionIcons::kExport, ImVec2(size, size)))
			PresetExportPanel::Open();
		Util::AddTooltip(T("cs_editor.export_preset", "Export Preset..."), Util::kTooltipWhenDisabled);
	}
	DrawManageSettings();
	using ContextType = SceneSettingsManager::SceneContextType;
	const char* labels[] = { T("feature.scene_manager.tab.interior", "Interior"),
		T("feature.scene_manager.tab.time_of_day", "Time of Day"), T("feature.scene_manager.tab.locations", "Locations"),
		T("feature.scene_manager.tab.weather", "Weather"), T("feature.scene_manager.transitions", "Transition Defaults") };
	static int previousPage = -1;
	{
		const Util::DisableGuard disabled(SceneEntryTable::HasActiveDialog());
		Util::SegmentedControl("##SceneManagerPages", labels, IM_ARRAYSIZE(labels), standalonePage);
	}
	if (previousPage != standalonePage) {
		SceneEntryTable::EndPresetEditing();
		weatherPickerScroll = 0.0f;
		locationPickerScroll = 0.0f;
		previousPage = standalonePage;
	}
	ImGui::Spacing();
	auto* manager = SceneSettingsManager::GetSingleton();
	switch (standalonePage) {
	case 0:
		ImGui::TextUnformatted(T("feature.scene_manager.interior.title", "Interior Settings"));
		ImGui::Separator();
		SceneEntryTable::Draw({ .type = ContextType::Interior });
		break;
	case 1:
		{
			ImGui::TextUnformatted(T("feature.scene_manager.time_of_day.title", "Time of Day Settings"));
			const auto* exteriorLabel = T("feature.scene_manager.time_of_day.exterior_only", "(Exterior Only)");
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(exteriorLabel).x);
			ImGui::TextDisabled("%s", exteriorLabel);
			auto currentPeriod = SceneSettingsManager::GetCurrentPeriod();
			const auto* currentPeriodName = SceneSettingsContextRules::GetCopyPeriodName(currentPeriod);
			const float currentGameHour = SceneSettingsManager::GetCurrentGameHour();
			auto currentTimeLabel = std::vformat(
				T("feature.scene_manager.time_of_day.current_time", "[{0} {1:.1f} h]"),
				std::make_format_args(currentPeriodName, currentGameHour));
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(currentTimeLabel.c_str()).x);
			ImGui::TextColored(globals::menu->GetTheme().StatusPalette.InfoColor, "%s", currentTimeLabel.c_str());
			ImGui::Separator();
			SceneEntryTable::Draw({ .type = ContextType::TimeOfDay, .period = currentPeriod });
			break;
		}
	case 2:
	case 3:
		{
			const auto selected = GetStandaloneContext();
			auto periodic = selected && manager->IsSceneTimeOfDayEnabled(*selected);
			{
				const Util::DisableGuard disabled(!selected || SceneEntryTable::HasActiveDialog() ||
												  manager->IsContextEditModeTarget(*selected));
				if (ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day", "Time of Day"), &periodic) && selected) {
					if (selected->type == ContextType::Location)
						manager->AddLocationTarget(*standaloneLocation);
					manager->SetSceneTimeOfDayEnabled(*selected, periodic);
				}
			}
			{
				const Util::DisableGuard lockTarget(SceneEntryTable::HasActiveDialog());
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (standalonePage == 2)
					DrawLocationPicker(standaloneLocation);
				else
					DrawWeatherPicker(standaloneWeather);
			}
			if (auto context = GetStandaloneContext()) {
				if (standaloneLocation && context->type == ContextType::Location && !standaloneLocation->cocCode.empty())
					ImGui::TextDisabled("%s: %s", T("feature.scene_manager.location.coc_code", "COC code"), standaloneLocation->cocCode.c_str());
				if (manager->IsSceneTimeOfDayEnabled(*context))
					context->period = SceneSettingsManager::GetCurrentPeriod();
				SceneEntryTable::Draw(*context);
			} else if (standalonePage == 2) {
				Util::Text::WrappedDisabled("%s", T("feature.scene_manager.location.no_target", "No location target is selected."));
				ImGui::TextWrapped("%s", T("feature.scene_manager.location.no_target_hint",
											 "Choose any loaded worldspace, region, location type, location, or named cell to add or manage its settings."));
			} else {
				Util::Text::WrappedDisabled("%s", T("feature.scene_manager.weather.no_target", "No weather is selected."));
				ImGui::TextWrapped("%s", T("feature.scene_manager.weather.no_target_hint",
											 "Choose any loaded weather to add or manage its settings."));
			}
			break;
		}
	case 4:
		globals::features::sceneManager.DrawTransitions();
		break;
	}
	DrawTargetManagement();
}

bool SceneSettingsUI::CanEditFeaturePage(Feature* feature)
{
	return feature && feature->loaded && LayerListsFeature(SceneSettingsManager::SceneContextType::Location, feature->GetShortName());
}

namespace
{
	SceneSettingsManager::SettingSelection GetPageSelection(Feature& feature)
	{
		if (!feature.HasScopedDefaultSettings())
			return std::nullopt;
		const auto keys = feature.GetCurrentPageSettingKeys();
		std::set<SceneSettingsManager::SettingIdentity> selected;
		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (setting.featureShortName != feature.GetShortName())
				continue;
			const auto path = Util::Settings::SplitCatalogPath(setting.serializedPath);
			const auto root = path.empty() ? setting.serializedKey : std::string_view(path.front());
			if (std::ranges::find(keys, root) != keys.end())
				selected.insert({ std::string(setting.featureShortName), SceneSettingsManager::SplitSettingPath(setting.settingPath), std::string(setting.settingKey) });
		}
		return selected;
	}

	std::optional<SceneSettingsManager::SceneContextId> GetInitialPageContext(Feature& feature)
	{
		using Type = SceneSettingsManager::SceneContextType;
		const auto name = feature.GetShortName();
		if (const auto context = InitialFeatureContext(Util::IsInterior() ? Type::Interior : Type::Weather, name))
			return context;
		return InitialFeatureContext(Type::Location, name);
	}
}

bool SceneSettingsUI::BeginFeaturePageEditing(Feature* feature)
{
	if (!CanEditFeaturePage(feature))
		return false;
	const auto name = feature->GetShortName();
	featurePageKeys = feature->HasScopedDefaultSettings() ? std::optional{ feature->GetCurrentPageSettingKeys() } : std::nullopt;
	featurePageSelection = GetPageSelection(*feature);
	++featurePageScopeRevision;
	const auto context = GetInitialPageContext(*feature);
	if (!context)
		return false;
	RequestFeatureContext(name, *context);
	return IsFeaturePageEditing(feature);
}

bool SceneSettingsUI::IsFeaturePageEditing(Feature* feature)
{
	return feature && featureToolbarOpen && featurePageContext && featurePageName == feature->GetShortName();
}

void SceneSettingsUI::UpdateFeaturePage(Feature* feature)
{
	if (feature != &globals::features::sceneManager)
		SceneEntryTable::EndPresetEditing();
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!IsFeaturePageEditing(feature)) {
		if (manager->IsContextEditing(featurePageName))
			manager->DiscardContextEdit();
		return;
	}
	const auto keys = feature->HasScopedDefaultSettings() ? std::optional{ feature->GetCurrentPageSettingKeys() } : std::nullopt;
	if (keys != featurePageKeys) {
		HideFeaturePageEditing();
		BeginFeaturePageEditing(feature);
	}
	if (!manager->IsContextEditPreviewActive() || !manager->IsContextEditing(featurePageName))
		manager->BeginContextEdit(featurePageName, *featurePageContext);
}

void SceneSettingsUI::HideFeaturePageEditing()
{
	++featurePageScopeRevision;
	featureToolbarOpen = false;
	SceneSettingsManager::GetSingleton()->DiscardContextEdit();
	Util::EnvironmentControls::StopPreview();
}

const char* SceneSettingsUI::PendingContextEditMessage()
{
	return T("feature.scene_manager.edit.finish_pending", "Save or discard unsaved scene settings before copying them, changing presets, or reloading.");
}

void SceneSettingsUI::WarnPendingContextEdit()
{
	EditorWindow::GetSingleton()->ShowNotification(PendingContextEditMessage(), Util::Colors::GetWarning());
}

bool SceneSettingsUI::SetFeaturePagePreviewPlaying(bool playing)
{
	if (!playing) {
		Util::EnvironmentControls::StopPreview();
		return true;
	}
	if (!featureToolbarOpen || !featurePageContext)
		return false;
	const auto& context = *featurePageContext;
	if (!CanPreviewContext(context))
		return false;
	auto* weather = context.type == SceneSettingsManager::SceneContextType::Weather ?
	                    RE::TESForm::LookupByID<RE::TESWeather>(context.weatherId) :
	                    nullptr;
	const auto hour = context.period != TimeOfDayPeriod::Count ?
	                      std::optional{ SceneSettingsManager::GetSingleton()->GetPeriodMidHour(context.period) } :
	                      std::nullopt;
	if (weather || hour) {
		if (!Util::EnvironmentControls::StartPreview(weather, hour))
			return false;
	} else {
		Util::EnvironmentControls::StopPreview();
	}
	if (context.type == SceneSettingsManager::SceneContextType::Location)
		if (const auto travel = GetLocationTravelCommand(context))
			SKSE::GetTaskInterface()->AddTask([command = *travel] { RE::Console::ExecuteCommand(command.c_str()); });
	return true;
}

bool SceneSettingsUI::DrawFeaturePageControls(Feature* feature, bool enabled)
{
	if (!CanEditFeaturePage(feature))
		return false;
	const auto name = feature->GetShortName();
	DrawFeatureContextConfirmation(name);
	if (!enabled || !IsFeaturePageEditing(feature))
		return false;
	WatchSceneSettingsForUndo();
	auto* manager = SceneSettingsManager::GetSingleton();
	using ContextType = SceneSettingsManager::SceneContextType;
	constexpr std::array types{ ContextType::Interior, ContextType::TimeOfDay, ContextType::Weather, ContextType::Location };
	const char* labels[] = { T("feature.scene_manager.tab.interior", "Interior"), T("feature.scene_manager.tab.time_of_day", "Time of Day"),
		T("feature.scene_manager.tab.weather", "Weather"), T("feature.scene_manager.tab.locations", "Locations") };
	std::array<SceneSettingMarker, types.size()> typeMarkers;
	float typeWidth = 0.0f;
	for (std::size_t index = 0; index < types.size(); ++index) {
		typeMarkers[index] = GetFeatureSceneMarker(name, { .type = types[index] }, false, true);
		typeWidth = std::max(typeWidth, ImGui::CalcTextSize(labels[index]).x);
	}
	typeWidth = ComboWidth(typeWidth);
	const float actionWidth = BrowserUI::IconButtonSize();
	const float cellPadding = ImGui::GetStyle().ItemInnerSpacing.x;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(cellPadding * 0.5f, ImGui::GetStyle().CellPadding.y));
	const SKSE::stl::scope_exit restorePadding([]() noexcept { ImGui::PopStyleVar(); });
	const float periodWidth = GetPeriodPickerWidth(*featurePageContext);
	const bool hasTargetPicker = featurePageContext->type == ContextType::Weather || featurePageContext->type == ContextType::Location;
	const bool hasPeriodPicker = featurePageContext->type != ContextType::Interior;
	const int pickerColumns = hasPeriodPicker ? 2 : 1;
	constexpr int actionColumns = 6;
	const int inlineColumns = pickerColumns + actionColumns + (hasTargetPicker ? 0 : 1);
	const float targetWidth = periodWidth + (hasTargetPicker ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f);
	const bool singleRow = ImGui::GetContentRegionAvail().x >= typeWidth + targetWidth + actionWidth * actionColumns + cellPadding * inlineColumns;
	if (ImGui::BeginTable("##FeatureSceneEditorBar", singleRow ? inlineColumns : pickerColumns, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("##type", ImGuiTableColumnFlags_WidthFixed, typeWidth);
		if (hasPeriodPicker) {
			if (hasTargetPicker || !singleRow)
				ImGui::TableSetupColumn("##target", ImGuiTableColumnFlags_WidthStretch);
			else
				ImGui::TableSetupColumn("##target", ImGuiTableColumnFlags_WidthFixed, periodWidth);
		}
		if (singleRow) {
			for (int column = 0; column < actionColumns - 1; ++column)
				ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, actionWidth);
			if (!hasTargetPicker)
				ImGui::TableSetupColumn("##space", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##close", ImGuiTableColumnFlags_WidthFixed, actionWidth);
		}
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto selected = std::ranges::find(types, featurePageContext->type) - types.begin();
		if (BeginScenePickerCombo("##FeatureSceneType", labels[selected], false, typeMarkers[selected])) {
			for (int index = 0; index < IM_ARRAYSIZE(labels); ++index) {
				auto context = InitialFeatureContext(types[index], name);
				const Util::DisableGuard disabled(!context);
				ImGui::PushStyleColor(ImGuiCol_Text, GetScenePickerTextColor(false, typeMarkers[index]));
				if (Util::RoundedSelectable(labels[index], selected == index) && context)
					RequestFeatureContext(name, *context);
				ImGui::PopStyleColor();
				ReportScenePickerHint(false, typeMarkers[index]);
				if (selected == index)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (hasPeriodPicker) {
			ImGui::TableSetColumnIndex(1);
			DrawFeatureTargetPicker(name, *featurePageContext, periodWidth);
		}
		if (!singleRow) {
			ImGui::EndTable();
			ImGui::BeginGroup();
		}
		const auto nextAction = [&](int column) {
			if (singleRow)
				ImGui::TableSetColumnIndex(pickerColumns + column);
			else if (column > 0)
				BrowserUI::SameLineIfFits(actionWidth);
		};
		nextAction(0);
		const bool playing = Util::EnvironmentControls::IsPreviewActive();
		{
			const Util::DisableGuard disabled(!playing && !CanPreviewContext(*featurePageContext));
			if (BrowserUI::IconButton("##Preview", playing ? Icons::TI(ICON_TI_PLAYER_STOP) : SceneActionIcons::kResume,
					playing ? T("feature.scene_manager.edit.stop_preview", "Stop preview and release weather/time locks") :
							  T("feature.scene_manager.edit.play_preview", "Preview the selected scene and lock its weather/time"),
					playing))
				SetFeaturePagePreviewPlaying(!playing);
		}
		nextAction(1);
		{
			const Util::DisableGuard disabled(!manager->HasPendingContextEdit());
			if (BrowserUI::IconButton("##Save", SceneActionIcons::kSave, T("feature.scene_manager.action.save", "Save")))
				featureSaveFailed = !manager->SaveContextEdit();
		}
		nextAction(2);
		{
			const Util::DisableGuard disabled(manager->HasPendingContextEdit());
			if (BrowserUI::IconButton("##Copy", SceneActionIcons::kCopy, T("ui.copy", "Copy")))
				SceneCopyModal::Open(*featurePageContext, name, featurePageSelection);
		}
		using Source = SceneSettingsManager::EntrySource;
		const auto scene = *featurePageContext;
		SceneEntryTable::BeginFeaturePresetFrame(scene, name, featurePageScopeRevision);
		const auto isPresetTarget = [&](const SceneSettingsManager::SettingEntry& entry) {
			return entry.source == Source::Overwrite && entry.featureShortName == name &&
			       SceneSettingsContextRules::EntryBelongsToContext(entry, scene) &&
			       (!featurePageSelection || featurePageSelection->contains({ entry.featureShortName, entry.settingPath, entry.settingKey }));
		};
		const auto summary = manager->GetContextUserEntrySummary(scene, name, featurePageSelection);
		SceneSettingsManager::ContextEntrySummary presetSummary;
		bool hasPreset = false;
		for (const auto& entry : manager->GetContextEntries(scene))
			if (isPresetTarget(entry)) {
				hasPreset = true;
				if (entry.deleted)
					continue;
				++presetSummary.total;
				presetSummary.paused += entry.paused ? 1 : 0;
			}
		const bool hasUser = summary.total != 0;
		const bool canPausePreset = presetSummary.total != 0;
		const bool pending = manager->HasPendingContextEdit();
		nextAction(3);
		bool pausePressed;
		{
			const Util::DisableGuard disabled(pending || (!hasUser && !canPausePreset));
			const bool paused = (!hasUser || summary.AllPaused()) && (!canPausePreset || presetSummary.AllPaused());
			pausePressed = BrowserUI::IconButton("##Pause", paused ? SceneActionIcons::kResume : SceneActionIcons::kPause,
				paused ? T("feature.scene_manager.action.resume", "Resume") : T("feature.scene_manager.action.pause", "Pause"));
		}
		ChooseFeatureActionLayer("##PauseSceneLayer", pausePressed, hasUser && !pending, canPausePreset, pending, [&](Source source) {
			if (source == Source::User)
				manager->DiscardContextEdit();
			const auto& layer = source == Source::User ? summary : presetSummary;
			manager->SetContextEntriesPaused(scene, !layer.AllPaused(), name, featurePageSelection, source);
			if (source == Source::User)
				manager->BeginContextEdit(name, scene);
		});
		nextAction(4);
		bool deletePressed;
		{
			const Util::DisableGuard disabled(!hasUser && (!hasPreset || pending));
			deletePressed = BrowserUI::IconButton("##Delete", SceneActionIcons::kDelete, T("feature.scene_manager.action.delete", "Delete"));
		}
		ChooseFeatureActionLayer("##DeleteSceneLayer", deletePressed, hasUser, hasPreset, pending, [&](Source source) {
			if (source == Source::User) {
				featureDeleteConfirmation.title = T("feature.scene_manager.edit.delete_title", "Delete Scene Settings?");
				featureDeleteConfirmation.message = T("feature.scene_manager.edit.delete_message", "Delete this feature's User Settings from the selected scene?");
				featureDeleteConfirmation.confirmLabel = T("feature.scene_manager.action.delete", "Delete");
				featureDeleteConfirmation.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
				featureDeleteConfirmation.Request();
			} else {
				std::vector<const SceneSettingsManager::SettingEntry*> targets;
				for (const auto& entry : manager->GetContextEntries(scene))
					if (isPresetTarget(entry))
						targets.push_back(&entry);
				SceneEntryTable::RequestFeaturePresetDeletion(targets);
			}
		});
		if (singleRow) {
			ImGui::TableSetColumnIndex(inlineColumns - 1);
		} else {
			BrowserUI::SameLineIfFits(actionWidth);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - actionWidth));
		}
		if (BrowserUI::IconButton("##CloseSceneToolbar", Icons::TI(ICON_TI_X), T("feature.scene_manager.action.close", "Close")))
			HideFeaturePageEditing();
		if (singleRow)
			ImGui::EndTable();
		else
			ImGui::EndGroup();
	}
	if (featureDeleteConfirmation.Draw()) {
		manager->DiscardContextEdit();
		manager->ClearContextEntries(*featurePageContext, name, featurePageSelection);
		manager->BeginContextEdit(name, *featurePageContext);
	}
	SceneCopyModal::Draw(*featurePageContext, name);
	if (featureSaveFailed)
		Util::Text::WrappedError("%s", T("feature.scene_manager.edit.save_failed",
										   "Could not save scene settings. Your unsaved changes are kept. Try Save again."));
	ImGui::Separator();
	return true;
}

void SceneSettingsUI::DrawFeaturePageBody(Feature* feature)
{
	if (!IsFeaturePageEditing(feature))
		return;
	const auto name = feature->GetShortName();
	const auto context = *featurePageContext;
	SceneEntryTable::BeginFeaturePresetFrame(context, name, featurePageScopeRevision);
	SceneFeatureReplica::Draw(name, context);
	SceneEntryTable::FinishFeaturePresetFrame();
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager->IsContextEditing(name))
		manager->BeginContextEdit(name, context);
}

bool SceneSettingsUI::DrawWeatherTargetPicker(RE::FormID& selected, float& scroll)
{
	return DrawWeatherPicker(selected, {}, &scroll);
}

bool SceneSettingsUI::DrawLocationTargetPicker(std::optional<SceneSettingsManager::LocationTarget>& selected, float& scroll)
{
	return DrawLocationPicker(selected, {}, &scroll);
}
