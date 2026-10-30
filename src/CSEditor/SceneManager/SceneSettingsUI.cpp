#include "SceneSettingsUI.h"
#include "SceneSettingsContextRules.h"

#include <algorithm>
#include <array>
#include <cctype>
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
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/Icons/helpers/SceneActionIcons.h"
#include "SceneCopyModal.h"
#include "SceneEntryTable.h"
#include "SceneFeatureReplica.h"
#include "SceneManager.h"
#include "State.h"
#include "Utils/Form.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <imgui_stdlib.h>

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using TimeOfDayPeriod = SceneSettingsManager::TimeOfDayPeriod;
	constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;
	constexpr float kManageModalWidthEm = 48.0f;
	constexpr float kTargetManageWidthEm = 24.0f;
	std::string featurePageName;
	std::optional<SceneSettingsManager::SceneContextId> featurePageContext;
	bool featureToolbarOpen = false;
	bool featureSaveFailed = false;
	std::optional<std::pair<std::string, SceneSettingsManager::SceneContextId>> requestedFeatureContext;
	Util::ConfirmationPopup featureContextConfirmation;
	Util::ConfirmationPopup featureDeleteConfirmation;
	Util::DeferredModal manageSettingsModal;
	Util::ConfirmationPopup deleteSceneChangesConfirmation;
	SceneSettingsManager::EntryLayerScope pendingDeleteScope;
	int manageSection = 0;
	int manageScope = 0;
	int manageSource = 0;
	bool manageSelectedTarget = false;
	int standalonePage = 0;
	RE::FormID standaloneWeather = 0;
	std::optional<SceneSettingsManager::LocationTarget> standaloneLocation;

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
		static std::uint64_t cachedRevision = std::numeric_limits<std::uint64_t>::max();
		static std::map<std::tuple<SceneSettingsManager::SceneContextId, bool, bool>, SceneSettingMarker> markers;
		if (cachedFeature != feature || cachedRevision != manager->GetEntryPresentationRevision()) {
			cachedFeature = feature;
			cachedRevision = manager->GetEntryPresentationRevision();
			markers.clear();
		}
		auto [it, inserted] = markers.try_emplace(std::tuple{ context, matchPeriod, wholeType });
		if (inserted) {
			const auto summary = wholeType ? manager->GetFeatureSceneSummary(feature, context.type) :
			                                 manager->GetFeatureSceneSummary(feature, context, matchPeriod);
			it->second = summary.activeOverwrites ? SceneSettingMarker::Overwrite :
			             summary.total            ? SceneSettingMarker::Settings :
			                                        SceneSettingMarker::None;
		}
		return it->second;
	}

	std::string GetScenePickerLabel(std::string_view label, SceneSettingMarker marker)
	{
		return marker == SceneSettingMarker::Overwrite ? std::format("{} **", label) :
		       marker == SceneSettingMarker::Settings  ? std::format("{} *", label) :
		                                                 std::string(label);
	}

	ImVec4 GetScenePickerTextColor(bool current, bool paused);
	bool DrawWeatherPicker(RE::FormID& selectedWeather, std::string_view feature = {}, float* scroll = nullptr);
	bool DrawLocationPicker(std::optional<SceneSettingsManager::LocationTarget>& selectedLocation, std::string_view feature = {}, float* scroll = nullptr);

	float GetPeriodPickerWidth()
	{
		float labelWidth = ImGui::CalcTextSize(T("feature.scene_manager.weather.normal", "Normal")).x;
		for (int period = 0; period < kPeriodCount; ++period)
			labelWidth = std::max(labelWidth, ImGui::CalcTextSize(GetPeriodLabel(period)).x);
		return labelWidth + ImGui::CalcTextSize(" **").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
	}

	void DrawFeatureTargetPicker(const std::string& name, SceneSettingsManager::SceneContextId context)
	{
		using Type = SceneSettingsManager::SceneContextType;
		if (context.type == Type::Interior) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(T("feature.scene_manager.tab.interior", "Interior"));
			return;
		}
		const bool periodicTarget = context.type == Type::Weather || context.type == Type::Location;
		const float available = ImGui::GetContentRegionAvail().x;
		const float periodWidth = std::min(available, GetPeriodPickerWidth());
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
			ImGui::SetNextItemWidth(periodWidth);
		} else {
			ImGui::SetNextItemWidth(-FLT_MIN);
		}
		const char* preview = context.period == TimeOfDayPeriod::Count ?
		                          T("feature.scene_manager.weather.normal", "Normal") :
		                          GetPeriodLabel(static_cast<int>(context.period));
		const auto previewLabel = GetScenePickerLabel(preview, GetFeatureSceneMarker(name, context));
		if (ImGui::BeginCombo("##ScenePeriod", previewLabel.c_str())) {
			auto* manager = SceneSettingsManager::GetSingleton();
			static std::uint64_t cachedRevision = std::numeric_limits<std::uint64_t>::max();
			static std::map<SceneSettingsManager::SceneContextId, bool> pausedPeriods;
			if (cachedRevision != manager->GetEntryPresentationRevision()) {
				cachedRevision = manager->GetEntryPresentationRevision();
				pausedPeriods.clear();
			}
			const auto drawPeriod = [&](TimeOfDayPeriod period, const char* label) {
				auto candidate = context;
				candidate.period = period;
				auto [it, inserted] = pausedPeriods.try_emplace(candidate);
				if (inserted)
					it->second = manager->GetFeatureSceneSummary({}, candidate).paused != 0;
				const auto text = GetScenePickerLabel(label, GetFeatureSceneMarker(name, candidate));
				ImGui::PushID(static_cast<int>(period));
				ImGui::PushStyleColor(ImGuiCol_Text, GetScenePickerTextColor(period == SceneSettingsManager::GetCurrentPeriod(), it->second));
				if (Util::RoundedSelectable(text.c_str(), context.period == period))
					RequestFeatureContext(name, candidate);
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

	bool IsConsoleIdentifier(std::string_view value)
	{
		return !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
			return std::isalnum(character) != 0 || character == '_';
		});
	}

	std::optional<std::string> GetCellTravelCommand(RE::TESObjectCELL* cell)
	{
		if (!cell)
			return std::nullopt;
		const auto cocCode = Util::GetFormEditorID(cell);
		if (IsConsoleIdentifier(cocCode))
			return std::format("coc {}", cocCode);
		if (!cell->IsExteriorCell())
			return std::nullopt;
		auto* coordinates = cell->GetCoordinates();
		auto* worldSpace = cell->GetRuntimeData().worldSpace;
		const auto worldSpaceEditorId = Util::GetFormEditorID(worldSpace);
		if (!coordinates || !IsConsoleIdentifier(worldSpaceEditorId))
			return std::nullopt;
		return std::format("cow {} {} {}", worldSpaceEditorId, coordinates->cellX, coordinates->cellY);
	}

	std::optional<std::string> GetLocationTravelCommand(const SceneSettingsManager::SceneContextId& context)
	{
		const auto& targets = SceneSettingsManager::GetSingleton()->GetLocationCatalog();
		const auto target = std::ranges::find_if(targets, [&](const auto& candidate) {
			return candidate.type == context.locationType && candidate.formKey == context.locationFormKey;
		});
		if (target == targets.end())
			return std::nullopt;
		if (IsConsoleIdentifier(target->cocCode))
			return std::format("coc {}", target->cocCode);
		RE::TESObjectCELL* cell = nullptr;
		if (target->type == SceneSettingsManager::LocationTargetType::Cell)
			cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(target->formId);
		else if (target->type == SceneSettingsManager::LocationTargetType::Location)
			if (auto* location = RE::TESForm::LookupByID<RE::BGSLocation>(target->formId))
				if (auto marker = location->worldLocMarker ? location->worldLocMarker.get() : nullptr)
					cell = marker->GetParentCell();
		return GetCellTravelCommand(cell);
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
		Manager::EntryLayerScope scope;
		if (target) {
			scope.type = target->type;
			scope.target = target;
		} else {
			const char* scopeLabels[] = { T(TKEY("scene_manage_scope_all"), "All scenes"),
				T("feature.scene_manager.tab.interior", "Interior"), T("feature.scene_manager.tab.time_of_day", "Time of Day"),
				T("feature.scene_manager.tab.locations", "Locations"), T("feature.scene_manager.tab.weather", "Weather") };
			const std::array types{ Type::Interior, Type::TimeOfDay, Type::Location, Type::Weather };
			ImGui::TextUnformatted(T(TKEY("scene_manage_scope"), "Scope"));
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::Combo("##SceneManageScope", &manageScope, scopeLabels, IM_ARRAYSIZE(scopeLabels));
			if (manageScope > 0)
				scope.type = types[manageScope - 1];
			const auto selected = GetStandaloneContext();
			if (selected && scope.type && selected->type == *scope.type &&
				(*scope.type == Type::Location || *scope.type == Type::Weather)) {
				ImGui::Checkbox(T(TKEY("scene_manage_selected_target"), "Selected target only"), &manageSelectedTarget);
				if (manageSelectedTarget) {
					scope.target = selected;
					Util::Text::WrappedDisabled("%s", manager->GetSceneContextDisplayName(*selected).c_str());
				}
			}
		}
		if (manager->HasPendingContextEdit())
			Util::Text::WrappedWarning("%s", SceneSettingsUI::PendingContextEditMessage());
		const char* sourceLabels[] = { T("feature.scene_manager.section.user_settings", "User Settings"),
			T("feature.scene_manager.section.overwrite_files", "Preset Settings") };
		ImGui::Spacing();
		Util::SegmentedControl("##SceneManageSource", sourceLabels, IM_ARRAYSIZE(sourceLabels), manageSource);
		const auto source = manageSource == 0 ? Source::User : Source::Overwrite;
		{
			ImGui::PushID(static_cast<int>(source));
			const auto summary = manager->GetEntryLayerSummary(source, scope);
			auto active = summary.total - summary.paused;
			auto total = summary.total;
			ImGui::TextUnformatted(std::vformat(T(TKEY("scene_manage_active_count"), "{} active / {} total settings"),
				std::make_format_args(active, total))
					.c_str());
			if (source == Source::Overwrite)
				Util::Text::WrappedDisabled("%s", T(TKEY("scene_manage_pause_session"), "Pausing installed settings lasts until they are reloaded."));
			ImGui::Spacing();
			const Util::DisableGuard disabled(manager->HasPendingContextEdit() || summary.total == 0);
			if (Util::Button(summary.AllPaused() ? T("feature.scene_manager.action.unpause_all", "Unpause All") :
												   T("feature.scene_manager.action.pause_all", "Pause All")))
				globals::menu->GetUndoHistory().Change(SceneSettingsUI::MakeUndoTarget(), [&] {
					manager->SetEntryLayerPaused(source, !summary.AllPaused(), scope);
				});
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(T("feature.scene_manager.action.delete_all", "Delete All")).x + ImGui::GetStyle().FramePadding.x * 2.0f);
			if (source == Source::User && Util::Button(T("feature.scene_manager.action.delete_all", "Delete All"))) {
				pendingDeleteScope = scope;
				deleteSceneChangesConfirmation.title = T(TKEY("scene_page_delete_changes_title"), "Delete changes");
				deleteSceneChangesConfirmation.message = T(TKEY("scene_manage_delete_user"), "Delete your settings in the selected scope? Installed settings remain. Undo restores your changes.");
				deleteSceneChangesConfirmation.confirmLabel = T("feature.scene_manager.action.delete", "Delete");
				deleteSceneChangesConfirmation.cancelLabel = T(TKEY("cancel"), "Cancel");
				deleteSceneChangesConfirmation.Request();
			}
			ImGui::PopID();
		}
	}

	void DrawLayerDeleteConfirmation()
	{
		if (deleteSceneChangesConfirmation.Draw()) {
			auto* manager = SceneSettingsManager::GetSingleton();
			globals::menu->GetUndoHistory().Change(SceneSettingsUI::MakeUndoTarget(), [manager] {
				manager->DeleteEntryLayer(SceneSettingsManager::EntrySource::User, pendingDeleteScope);
			});
		}
	}

	void DrawManageSettings()
	{
		const auto title = std::format("{}###SceneManagerSettings", T("feature.scene_manager.action.manage_settings", "Manage Scene Settings"));
		manageSettingsModal.Draw(title.c_str(), ThemeManager::Constants::Em(kManageModalWidthEm), [] {
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
		bool paused = false;
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
			const auto entries = manager->GetContextEntries(context);
			it->second = { std::move(label), !entries.empty(),
				std::ranges::any_of(entries, [](const auto& entry) { return entry.paused; }) };
		}
		return it->second;
	}

	ImVec4 GetScenePickerTextColor(bool current, bool paused)
	{
		if (paused)
			return Util::Colors::GetWarning();
		if (current)
			return Util::Colors::GetSuccess();
		return ImGui::GetStyleColorVec4(ImGuiCol_Text);
	}

	void DrawTargetManagementPopup(const SceneSettingsManager::SceneContextId& target, const std::string& label, bool open)
	{
		if (open) {
			ImGui::OpenPopup("##ManageSceneTarget");
		}
		ImGui::SetNextWindowSizeConstraints(ImVec2(ThemeManager::Constants::Em(kTargetManageWidthEm), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (auto popup = Util::Popup("##ManageSceneTarget", label.c_str())) {
			DrawLayerManagement(target);
			DrawLayerDeleteConfirmation();
		}
	}

	bool IsManagingSceneTarget(const char* id)
	{
		return ImGui::IsPopupOpen(ImHashStr("##ManageSceneTarget", 0, ImGui::GetID(id)), ImGuiPopupFlags_AnyPopupLevel);
	}

	bool DrawSceneTargetSelectable(const char* id, const ScenePickerPresentation& presentation, bool selected, bool current,
		const SceneSettingsManager::SceneContextId& context, std::string_view feature)
	{
		ImGui::PushID(id);
		const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
		const bool canManage = feature.empty() && (presentation.configured || ImGui::IsPopupOpen("##ManageSceneTarget"));
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
		const auto label = GetScenePickerLabel(presentation.label, GetFeatureSceneMarker(feature, context, false));
		const ImVec4 clip(start.x, start.y, canManage ? buttonX - ImGui::GetStyle().ItemSpacing.x : right,
			start.y + ImGui::GetTextLineHeight());
		ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), start,
			ImGui::GetColorU32(GetScenePickerTextColor(current, presentation.paused)), label.c_str(), nullptr, 0.0f, &clip);
		if (canManage) {
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(buttonX, start.y));
			const bool open = ImGui::SmallButton(manageLabel);
			DrawTargetManagementPopup(context, presentation.label, open);
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
		bool& configuredOnly = filters[std::string(feature)];
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

		if (!Util::BeginSearchableCombo(feature.empty() ? T("feature.scene_manager.weather.target", "Weather") : "##FeatureSceneWeather", GetScenePickerLabel(preview, GetFeatureSceneMarker(feature, { .type = Type::Weather, .weatherId = selectedWeather }, false)).c_str(),
				ImGuiComboFlags_None, nullptr, kSceneTargetComboVisibleItems, scroll ? scroll : &weatherPickerScroll))
			return false;
		std::vector<RE::TESWeather*> current;
		std::vector<RE::TESWeather*> configured;
		std::vector<RE::TESWeather*> available;
		const auto isCurrent = [](const RE::TESWeather* weather) {
			const auto* sky = globals::game::sky;
			return sky && (sky->currentWeather == weather || sky->lastWeather == weather);
		};
		for (auto* weather : targets) {
			const auto& presentation = GetScenePickerPresentation({ .type = Type::Weather, .weatherId = weather->GetFormID() },
				GetWeatherTargetLabel(weather));
			(presentation.configured ? configured : isCurrent(weather) ? current :
																		 available)
				.push_back(weather);
		}
		const auto drawGroup = [&](const char* label, const std::vector<RE::TESWeather*>& candidates) {
			const bool showFilter = !feature.empty() && &candidates == &configured;
			if (showFilter) {
				ImGui::SeparatorText(label);
				ImGui::Checkbox(T("feature.scene_manager.edit.configured_only", "Feature Specific"), &configuredOnly);
				Util::AddTooltip(T("feature.scene_manager.edit.configured_only.tooltip", "Only show scenes with saved settings for this feature."));
			}
			if (!feature.empty() && configuredOnly && !showFilter)
				return;
			std::vector<RE::TESWeather*> visible;
			int selectedIndex = -1;
			int managedIndex = -1;
			for (auto* weather : candidates) {
				const auto& presentation = GetScenePickerPresentation({ .type = Type::Weather, .weatherId = weather->GetFormID() },
					GetWeatherTargetLabel(weather));
				const bool managing = feature.empty() && IsManagingSceneTarget(std::format("CopyWeather{:08X}", weather->GetFormID()).c_str());
				if ((!managing && !Util::SearchableComboMatches(presentation.label)) || (!feature.empty() && configuredOnly &&
																							GetFeatureSceneMarker(feature, { .type = Type::Weather, .weatherId = weather->GetFormID() }, false) == SceneSettingMarker::None))
					continue;
				if (weather->GetFormID() == selectedWeather)
					selectedIndex = static_cast<int>(visible.size());
				if (managing)
					managedIndex = static_cast<int>(visible.size());
				visible.push_back(weather);
			}
			if (visible.empty())
				return;
			if (!showFilter)
				ImGui::SeparatorText(label);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visible.size()));
			if (selectedIndex >= 0)
				clipper.IncludeItemByIndex(selectedIndex);
			if (managedIndex >= 0)
				clipper.IncludeItemByIndex(managedIndex);
			while (clipper.Step())
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
					auto* weather = visible[index];
					const Context context{ .type = Type::Weather, .weatherId = weather->GetFormID() };
					const auto& presentation = GetScenePickerPresentation(context, GetWeatherTargetLabel(weather));
					const auto id = std::format("CopyWeather{:08X}", weather->GetFormID());
					if (DrawSceneTargetSelectable(id.c_str(), presentation, weather->GetFormID() == selectedWeather, isCurrent(weather), context, feature)) {
						selectedWeather = weather->GetFormID();
						changed = true;
					}
				}
		};
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
		bool& configuredOnly = filters[std::string(feature)];
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

		if (!Util::BeginSearchableCombo(feature.empty() ? T("feature.scene_manager.location.target", "Target") : "##FeatureSceneLocation", GetScenePickerLabel(preview, selectedLocation ? GetFeatureSceneMarker(feature, { .type = ContextType::Location, .locationType = selectedLocation->type, .locationFormKey = selectedLocation->formKey }, false) : SceneSettingMarker::None).c_str(),
				ImGuiComboFlags_None, nullptr, kSceneTargetComboVisibleItems, scroll ? scroll : &locationPickerScroll))
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
			const bool showFilter = !feature.empty() && &candidates == &configured;
			if (showFilter) {
				ImGui::SeparatorText(label);
				ImGui::Checkbox(T("feature.scene_manager.edit.configured_only", "Feature Specific"), &configuredOnly);
				Util::AddTooltip(T("feature.scene_manager.edit.configured_only.tooltip", "Only show scenes with saved settings for this feature."));
			}
			std::vector<const Target*> visible;
			int selectedIndex = -1;
			int managedIndex = -1;
			for (const auto* target : candidates) {
				const bool managing = feature.empty() && IsManagingSceneTarget(std::format("{}:{}", static_cast<int>(target->type), target->formKey).c_str());
				if ((!managing && !Util::SearchableComboMatches(getPresentation(*target).label)) || (!feature.empty() && configuredOnly &&
																										GetFeatureSceneMarker(feature, { .type = ContextType::Location, .locationType = target->type, .locationFormKey = target->formKey }, false) == SceneSettingMarker::None))
					continue;
				if (selectedLocation && IsSameLocationTarget(*selectedLocation, *target))
					selectedIndex = static_cast<int>(visible.size());
				if (managing)
					managedIndex = static_cast<int>(visible.size());
				visible.push_back(target);
			}
			if (visible.empty())
				return;
			if (types && Util::GetSearchableComboFilter().empty()) {
				ImGui::SeparatorText("");
				if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoTreePushOnOpen))
					return;
			} else if (!showFilter) {
				ImGui::SeparatorText(label);
			}
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visible.size()));
			if (selectedIndex >= 0)
				clipper.IncludeItemByIndex(selectedIndex);
			if (managedIndex >= 0)
				clipper.IncludeItemByIndex(managedIndex);
			while (clipper.Step())
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
					const auto& target = *visible[index];
					const auto id = std::format("{}:{}", static_cast<int>(target.type), target.formKey);
					if (DrawSceneTargetSelectable(id.c_str(), getPresentation(target),
							selectedLocation && IsSameLocationTarget(*selectedLocation, target), isCurrent(target),
							{ .type = ContextType::Location, .locationType = target.type, .locationFormKey = target.formKey }, feature)) {
						selectedLocation = target;
						changed = true;
					}
				}
		};
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
		const auto context = GetStandaloneContext().value_or(SceneSettingsManager::SceneContextId{
			.type = SceneSettingsManager::SceneContextType::TimeOfDay,
			.period = SceneSettingsManager::GetCurrentPeriod() });
		SceneCopyModal::PrepareEmbedded(context);
		manageSettingsModal.Request();
	}
	DrawManageSettings();
	using ContextType = SceneSettingsManager::SceneContextType;
	const char* labels[] = { T("feature.scene_manager.tab.interior", "Interior"),
		T("feature.scene_manager.tab.time_of_day", "Time of Day"), T("feature.scene_manager.tab.locations", "Locations"),
		T("feature.scene_manager.tab.weather", "Weather"), T("feature.scene_manager.transitions", "Transitions") };
	static int previousPage = -1;
	{
		const Util::DisableGuard disabled(SceneEntryTable::HasActiveDialog());
		Util::SegmentedControl("##SceneManagerPages", labels, IM_ARRAYSIZE(labels), standalonePage);
	}
	if (previousPage != standalonePage) {
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
		{
			{
				const Util::DisableGuard lockTarget(SceneEntryTable::HasActiveDialog());
				ImGui::SetNextItemWidth(-FLT_MIN);
				DrawLocationPicker(standaloneLocation);
			}
			if (!standaloneLocation) {
				Util::Text::WrappedDisabled("%s", T("feature.scene_manager.location.no_target", "No location target is selected."));
				ImGui::TextWrapped("%s", T("feature.scene_manager.location.no_target_hint",
											 "Choose any loaded worldspace, region, location type, location, or named cell to add or manage its settings."));
				break;
			}
			const auto& target = *standaloneLocation;
			ImGui::TextDisabled("%s: %s", T("feature.scene_manager.location.spid_key", "SPID key"), target.formKey.c_str());
			if (!target.cocCode.empty())
				ImGui::TextDisabled("%s: %s", T("feature.scene_manager.location.coc_code", "COC code"), target.cocCode.c_str());
			SceneSettingsManager::SceneContextId context{ .type = ContextType::Location,
				.locationType = target.type,
				.locationFormKey = target.formKey };
			auto periodic = manager->IsSceneTimeOfDayEnabled(context);
			ImGui::BeginDisabled(SceneEntryTable::HasActiveDialog() || manager->IsContextEditModeTarget(context));
			if (ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day", "Time of Day"), &periodic)) {
				manager->AddLocationTarget(target);
				manager->SetSceneTimeOfDayEnabled(context, periodic);
			}
			ImGui::EndDisabled();
			if (periodic)
				context.period = SceneSettingsManager::GetCurrentPeriod();
			SceneEntryTable::Draw(context);
			break;
		}
	case 3:
		{
			{
				const Util::DisableGuard lockTarget(SceneEntryTable::HasActiveDialog());
				ImGui::SetNextItemWidth(-FLT_MIN);
				DrawWeatherPicker(standaloneWeather);
			}
			if (!standaloneWeather) {
				Util::Text::WrappedDisabled("%s", T("feature.scene_manager.weather.no_target", "No weather is selected."));
				ImGui::TextWrapped("%s", T("feature.scene_manager.weather.no_target_hint",
											 "Choose any loaded weather to add or manage its settings."));
				break;
			}
			ImGui::TextDisabled("%s: %s", T("feature.scene_manager.weather.spid_key", "SPID key"),
				Util::FormIdToSpid(standaloneWeather).c_str());
			SceneSettingsManager::SceneContextId context{ .type = ContextType::Weather, .weatherId = standaloneWeather };
			auto periodic = manager->IsSceneTimeOfDayEnabled(context);
			ImGui::BeginDisabled(SceneEntryTable::HasActiveDialog() || manager->IsContextEditModeTarget(context));
			if (ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day", "Time of Day"), &periodic))
				manager->SetSceneTimeOfDayEnabled(context, periodic);
			ImGui::EndDisabled();
			if (periodic)
				context.period = SceneSettingsManager::GetCurrentPeriod();
			SceneEntryTable::Draw(context);
			break;
		}
	case 4:
		globals::features::sceneManager.DrawTransitions();
		break;
	}
}

bool SceneSettingsUI::CanEditFeaturePage(Feature* feature)
{
	return feature && feature->loaded && LayerListsFeature(SceneSettingsManager::SceneContextType::Location, feature->GetShortName());
}

bool SceneSettingsUI::BeginFeaturePageEditing(Feature* feature)
{
	if (!CanEditFeaturePage(feature))
		return false;
	const auto name = feature->GetShortName();
	if (featurePageName == name && featurePageContext && SceneSettingsManager::GetSingleton()->IsContextEditing(name))
		return StartFeatureContext(name, *featurePageContext);
	using Type = SceneSettingsManager::SceneContextType;
	auto context = InitialFeatureContext(Util::IsInterior() ? Type::Interior : Type::TimeOfDay, name);
	if (!context)
		context = InitialFeatureContext(Type::Weather, name);
	if (!context)
		context = InitialFeatureContext(Type::Location, name);
	if (!context)
		return false;
	RequestFeatureContext(name, *context);
	return IsFeaturePageEditing(feature);
}

bool SceneSettingsUI::IsFeaturePageEditing(Feature* feature)
{
	return feature && featureToolbarOpen && featurePageContext && featurePageName == feature->GetShortName() &&
	       SceneSettingsManager::GetSingleton()->IsContextEditing(featurePageName);
}

void SceneSettingsUI::HideFeaturePageEditing()
{
	featureToolbarOpen = false;
	SceneSettingsManager::GetSingleton()->SuspendContextEdit();
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
			RE::Console::ExecuteCommand(travel->c_str());
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
	const std::array types{ ContextType::Interior, ContextType::TimeOfDay, ContextType::Weather, ContextType::Location };
	const char* labels[] = { T("feature.scene_manager.tab.interior", "Interior"), T("feature.scene_manager.tab.time_of_day", "Time of Day"),
		T("feature.scene_manager.tab.weather", "Weather"), T("feature.scene_manager.tab.locations", "Locations") };
	float typeWidth = 0.0f;
	for (const auto* label : labels)
		typeWidth = std::max(typeWidth, ImGui::CalcTextSize(label).x);
	typeWidth += ImGui::CalcTextSize(" **").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
	const float actionWidth = BrowserUI::IconButtonSize() + ImGui::GetStyle().ItemSpacing.x;
	const float cellPadding = ImGui::GetStyle().CellPadding.x * 2.0f;
	const float targetWidth = GetPeriodPickerWidth() + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
	const bool singleRow = ImGui::GetContentRegionAvail().x >= typeWidth + targetWidth + actionWidth * 5.0f + cellPadding * 7.0f;
	if (ImGui::BeginTable("##FeatureSceneEditorBar", singleRow ? 7 : 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("##type", ImGuiTableColumnFlags_WidthStretch, typeWidth);
		ImGui::TableSetupColumn("##target", ImGuiTableColumnFlags_WidthStretch, targetWidth);
		if (singleRow)
			for (int column = 2; column < 7; ++column)
				ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, actionWidth);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto selected = std::ranges::find(types, featurePageContext->type) - types.begin();
		const auto typeLabel = GetScenePickerLabel(labels[selected], GetFeatureSceneMarker(name, { .type = types[selected] }, false, true));
		if (ImGui::BeginCombo("##FeatureSceneType", typeLabel.c_str())) {
			for (int index = 0; index < IM_ARRAYSIZE(labels); ++index) {
				auto context = InitialFeatureContext(types[index], name);
				const Util::DisableGuard disabled(!context);
				const auto label = GetScenePickerLabel(labels[index], GetFeatureSceneMarker(name, { .type = types[index] }, false, true));
				if (Util::RoundedSelectable(label.c_str(), selected == index) && context)
					RequestFeatureContext(name, *context);
				if (selected == index)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::TableSetColumnIndex(1);
		DrawFeatureTargetPicker(name, *featurePageContext);
		if (!singleRow) {
			ImGui::EndTable();
			ImGui::BeginGroup();
		}
		const auto nextAction = [&](int column) {
			if (singleRow)
				ImGui::TableSetColumnIndex(column);
			else if (column > 2)
				BrowserUI::SameLineIfFits(actionWidth);
		};
		nextAction(2);
		const bool playing = Util::EnvironmentControls::IsPreviewActive();
		{
			const Util::DisableGuard disabled(!playing && !CanPreviewContext(*featurePageContext));
			if (BrowserUI::IconButton("##Preview", playing ? Icons::TI(ICON_TI_PLAYER_STOP) : SceneActionIcons::kResume,
					playing ? T("feature.scene_manager.edit.stop_preview", "Stop preview and release weather/time locks") :
							  T("feature.scene_manager.edit.play_preview", "Preview the selected scene and lock its weather/time"),
					playing))
				SetFeaturePagePreviewPlaying(!playing);
		}
		nextAction(3);
		{
			const Util::DisableGuard disabled(!manager->HasPendingContextEdit());
			if (BrowserUI::IconButton("##Save", SceneActionIcons::kSave, T("feature.scene_manager.action.save", "Save")))
				featureSaveFailed = !manager->SaveContextEdit();
		}
		nextAction(4);
		{
			const Util::DisableGuard disabled(manager->HasPendingContextEdit());
			if (BrowserUI::IconButton("##Copy", SceneActionIcons::kCopy, T("ui.copy", "Copy")))
				SceneCopyModal::Open(*featurePageContext, name);
		}
		const auto summary = manager->GetContextUserEntrySummary(*featurePageContext, name);
		nextAction(5);
		{
			const Util::DisableGuard disabled(manager->HasPendingContextEdit() || summary.total == 0);
			if (BrowserUI::IconButton("##Pause", summary.AllPaused() ? SceneActionIcons::kResume : SceneActionIcons::kPause,
					summary.AllPaused() ? T("feature.scene_manager.action.resume", "Resume") : T("feature.scene_manager.action.pause", "Pause"))) {
				manager->DiscardContextEdit();
				manager->SetContextEntriesPaused(*featurePageContext, !summary.AllPaused(), name);
				manager->BeginContextEdit(name, *featurePageContext);
			}
		}
		nextAction(6);
		{
			const Util::DisableGuard disabled(summary.total == 0);
			if (BrowserUI::IconButton("##Delete", SceneActionIcons::kDelete, T("feature.scene_manager.action.delete", "Delete"), false,
					BrowserUI::DestructiveIconColor())) {
				featureDeleteConfirmation.title = T("feature.scene_manager.edit.delete_title", "Delete Scene Settings?");
				featureDeleteConfirmation.message = T("feature.scene_manager.edit.delete_message", "Remove this feature's User Settings from the selected scene?");
				featureDeleteConfirmation.confirmLabel = T("feature.scene_manager.action.delete", "Delete");
				featureDeleteConfirmation.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
				featureDeleteConfirmation.Request();
			}
		}
		if (singleRow)
			ImGui::EndTable();
		else
			ImGui::EndGroup();
	}
	if (featureDeleteConfirmation.Draw()) {
		manager->DiscardContextEdit();
		manager->ClearContextEntries(*featurePageContext, name);
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
	if (IsFeaturePageEditing(feature))
		SceneFeatureReplica::Draw(feature->GetShortName(), *featurePageContext);
}

bool SceneSettingsUI::DrawWeatherTargetPicker(RE::FormID& selected, float& scroll)
{
	return DrawWeatherPicker(selected, {}, &scroll);
}

bool SceneSettingsUI::DrawLocationTargetPicker(std::optional<SceneSettingsManager::LocationTarget>& selected, float& scroll)
{
	return DrawLocationPicker(selected, {}, &scroll);
}
