#include "Widget.h"
#include "Menu/IconLoader.h"

#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../I18n/I18n.h"
#include "Browser/BrowserWidgets.h"
#include "EditorWindow.h"
#include "IconsTabler.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/Icons/helpers/SettingsActionIcons.h"
#include "Menu/Icons/helpers/WeatherTypeIcons.h"
#include "Menu/MenuIcons.h"
#include "SaveManager.h"
#include "Util.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include "Utils/UIAnim.h"
#include "imgui_internal.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	/** @brief Fills the one "{}" of a translated message with a name. */
	std::string FormatName(const char* message, const std::string& name)
	{
		return std::vformat(message, std::make_format_args(name));
	}

	/** @brief Tooltip of the action that opens another window on a widget. */
	const char* GetAnotherViewTooltip()
	{
		return T(TKEY("new_window_tooltip"), "Open another window on this form, to show another tab beside this one. Edits show in both.");
	}
}

void Widget::SetOpen(bool state)
{
	if (state) {
		View().open = true;
		EditorWindow::GetSingleton()->widgetsHidden = false;
	} else {
		for (auto& view : views)
			view.open = false;
	}
}

void Widget::Save()
{
	const auto savedDocument = js;
	bool saved = false;
	const SKSE::stl::scope_exit restoreDocument([&] {
		if (!saved) {
			js = savedDocument;
			const auto editorID = GetEditorID();
			EditorWindow::GetSingleton()->ShowNotification(
				std::vformat(T(TKEY("form_save_failed"), "Could not save settings for {}. Your unsaved changes are kept."), std::make_format_args(editorID)),
				Util::Colors::GetError());
		}
		TrackEdits();
	});
	try {
		SaveSettings();
		// Validate that we have valid JSON to write
		if (js.is_null()) {
			logger::warn("{}: Cannot save - JSON data is null", GetEditorID());
			return;
		}
		const auto file = GetSaveFilePath();
		if (!Util::FileHelpers::WriteJsonAtomically(file, js, 2, "editor widget settings")) {
			logger::error("{}: Failed to write settings file: {}", GetEditorID(), file);
			return;
		}
		CommitSavedSettings();
		saved = true;
		EditorWindow::GetSingleton()->OnWidgetJsonAttachmentChanged(this);
	} catch (const nlohmann::json::exception& e) {
		logger::error("{}: JSON error while saving settings: {}", GetEditorID(), e.what());
	} catch (const std::exception& e) {
		logger::error("{}: Unexpected error saving settings file: {}", GetEditorID(), e.what());
	}
}

void Widget::Load(bool showNotification)
{
	const auto path = FormEditSources::ResolveFile(GetFormKey());
	const auto editorID = GetEditorID();
	json document;
	if (!path.empty() && !FormEditSources::ReadDocument(path, document)) {
		logger::error("Could not read form settings '{}'", Util::PathHelpers::ToUtf8(path));
		if (showNotification)
			EditorWindow::GetSingleton()->ShowNotification(
				std::vformat(T(TKEY("form_load_failed"), "Could not load settings for {}. Check the log."), std::make_format_args(editorID)),
				Util::Colors::GetError());
		return;
	}
	if (!ApplySavedDocument(document))
		return;
	if (showNotification)
		EditorWindow::GetSingleton()->ShowNotification(
			std::vformat(path.empty() ? T(TKEY("form_load_vanilla"), "No saved file - reset {} to vanilla values") :
										T(TKEY("form_load_success"), "Loaded saved settings for {}"),
				std::make_format_args(editorID)),
			path.empty() ? Util::Colors::GetInfo() : Util::Colors::GetSuccess());
}

bool Widget::ApplySavedDocument(const json& document, bool updateSavedBaseline)
{
	const bool previousPreserve = preserveSavedBaseline;
	preserveSavedBaseline = !updateSavedBaseline;
	const SKSE::stl::scope_exit restoreBaselineMode([&] { preserveSavedBaseline = previousPreserve; });
	lastLoadSucceeded = true;
	js = document;
	try {
		LoadSettings();
	} catch (const std::exception& e) {
		logger::error("Could not load form settings for {}: {}", GetEditorID(), e.what());
		lastLoadSucceeded = false;
	}
	TrackEdits();
	return lastLoadSucceeded;
}

bool Widget::HasSettingsSource(SettingsSource source) const
{
	switch (source) {
	case SettingsSource::Game:
		return true;
	case SettingsSource::Preset:
		return FormEditSources::GetPackFormKeys().contains(GetFormKey());
	case SettingsSource::Saved:
		return HasSavedFile();
	}
	return false;
}

void Widget::RestoreSettings(SettingsSource source)
{
	if (!HasSettingsSource(source))
		return;
	auto* editor = EditorWindow::GetSingleton();
	if (source == SettingsSource::Game) {
		editor->undoHistory.Change(editor->MakeUndoTarget(*this), [this] { RevertChanges(); });
		return;
	}
	const auto path = source == SettingsSource::Preset ? FormEditSources::ResolvePackFile(GetFormKey()) : std::filesystem::path(GetSaveFilePath());
	json document;
	if (path.empty() || !FormEditSources::ReadDocument(path, document)) {
		editor->ShowNotification(FormatName(T(TKEY("form_load_failed"), "Could not load settings for {}. Check the log."), GetEditorID()), Util::Colors::GetError());
		return;
	}
	editor->undoHistory.Change(editor->MakeUndoTarget(*this), [&] {
		const auto before = CaptureEdits();
		const auto savedDocument = js;
		if (!ApplySavedDocument(document, false)) {
			js = savedDocument;
			if (before)
				RestoreEdits(*before);
			editor->ShowNotification(FormatName(T(TKEY("form_load_failed"), "Could not load settings for {}. Check the log."), GetEditorID()), Util::Colors::GetError());
			return;
		}
		if (source == SettingsSource::Saved)
			CommitSavedSettings();
	});
}

void Widget::TrackEdits()
{
	EditorWindow::GetSingleton()->TrackWidgetEdits(this);
}

void Widget::Delete()
{
	std::string filePath = GetSaveFilePath();

	if (!std::filesystem::exists(filePath)) {
		return;
	}

	try {
		std::filesystem::remove(filePath);

		// The active preset's file, when it has one, takes over from the deleted user file.
		Load(false);
		ApplyChanges();

		EditorWindow::GetSingleton()->OnWidgetJsonAttachmentChanged(this);

		auto editorId = GetEditorID();
		const char* message = FormEditSources::GetPackFormKeys().contains(GetFormKey()) ?
		                          T(TKEY("deleted_reverted_to_preset"), "Deleted {} - reverted to preset values") :
		                          T(TKEY("deleted_reverted_to_vanilla"), "Deleted {} - reverted to vanilla values");
		EditorWindow::GetSingleton()->ShowNotification(
			std::vformat(message, std::make_format_args(editorId)),
			Util::Colors::GetSuccess());
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("Error deleting settings file ({}) : {}\n", filePath, e.what());
	}
}

bool Widget::HasSavedFile() const
{
	return std::filesystem::exists(GetSaveFilePath());
}

std::optional<Widget::PackBadge> Widget::GetPackBadge() const
{
	if (!FormEditSources::GetPackFormKeys().contains(GetFormKey()))
		return std::nullopt;
	assert(globals::menu);
	const auto& statusPalette = globals::menu->GetTheme().StatusPalette;
	auto packName = FormEditSources::GetActivePackName();
	if (HasSavedFile())
		return PackBadge{ statusPalette.Warning,
			std::vformat(T(TKEY("pack_override_tooltip"), "Your saved file overrides preset '{}'. Delete it to use the preset's values."),
				std::make_format_args(packName)) };
	return PackBadge{ statusPalette.InfoColor,
		std::vformat(T(TKEY("pack_values_tooltip"), "Values from preset '{}'."), std::make_format_args(packName)) };
}

void Widget::DrawPackSourceBadge() const
{
	const auto packBadge = GetPackBadge();
	if (!packBadge)
		return;
	{
		Icons::FontGuard font(Icons::Family::Tabler);
		Util::SameLineIfFits(ImGui::CalcTextSize(ICON_TI_STACK_2).x);
		ImGui::TextColored(packBadge->color, "%s", ICON_TI_STACK_2);
	}
	Util::AddTooltip(packBadge->tooltip.c_str());
}

void Widget::DrawMenu()
{
	if (ImGui::BeginMenuBar()) {
		if (ImGui::BeginMenu(T(TKEY("menu"), "Menu"))) {
			if (Util::RoundedMenuItem(T(TKEY("save"), "Save"))) {
				SaveManager::SaveEditor(SaveManager::Trigger::User);
			}
			if (Util::RoundedMenuItem(T(TKEY("load"), "Load"))) {
				Load();
			}
			if (Util::RoundedMenuItem(T(TKEY("delete_saved_file"), "Delete Saved File"))) {
				ImGui::OpenPopup("DeleteConfirmation");
			}
			if (Util::RoundedMenuItem(T(TKEY("revert_to_game_values"), "Revert to Record Values"))) {
				RevertChanges();
			}

			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}

	DrawDeleteConfirmationModal();
}

void Widget::DrawDeleteConfirmationModal(const char* popupId)
{
	if (!ImGui::IsPopupOpen(popupId))
		return;
	if (deleteConfirmationFrame == ImGui::GetFrameCount())
		return;

	if (auto popup = Util::CenteredPopupModal(popupId)) {
		deleteConfirmationFrame = ImGui::GetFrameCount();
		ImGui::Text("%s", T(TKEY("confirm_delete_saved_file"), "Are you sure you want to delete the saved settings file?"));
		ImGui::Separator();

		const float scale = Util::GetUIScale();
		const float buttonWidth = ThemeManager::Constants::CSE_CONFIRM_BUTTON_WIDTH * scale;
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float totalWidth = (buttonWidth * 2) + spacing;
		const float cursorX = (ImGui::GetWindowWidth() - totalWidth) / 2.0f;

		ImGui::SetCursorPosX(cursorX);

		if (Util::Button(T(TKEY("yes_delete"), "Yes, Delete"), ImVec2(buttonWidth, 0))) {
			Delete();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();

		if (Util::Button(T(TKEY("cancel"), "Cancel"), ImVec2(buttonWidth, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
	}
}

std::string Widget::GetSaveFilePath() const
{
	return FormEditSources::GetFilePath(Util::PathHelpers::GetCommunityShaderPath(), GetFormKey()).string();
}

std::string Widget::GetFolderName() const
{
	switch (form->GetFormType()) {
	case RE::FormType::Weather:
		return std::string(kWeatherFolderName);
	case RE::FormType::LightingMaster:
		return std::string(kLightingTemplateFolderName);
	case RE::FormType::ImageSpace:
		return std::string(kImageSpaceFolderName);
	case RE::FormType::VolumetricLighting:
		return std::string(kVolumetricLightingFolderName);
	case RE::FormType::ShaderParticleGeometryData:
		return std::string(kPrecipitationFolderName);
	case RE::FormType::ReferenceEffect:
		return std::string(kVisualEffectsFolderName);
	case RE::FormType::Cell:
		return std::string(kCellLightingFolderName);
	case RE::FormType::LensFlare:
		return std::string(kLensFlareFolderName);
	default:
		return std::string(kOtherEditorWidgetsFolderName);
	}
}

void Widget::OpenAnotherView()
{
	// The lowest number no open window holds; closed ones linger until DrawViews prunes them.
	const auto taken = [this](int id) {
		return std::ranges::any_of(views, [id](const WidgetView& view) { return view.open && view.id == id; });
	};
	int id = 1;
	while (taken(id))
		++id;
	auto& view = views.emplace_back(WidgetView{ .id = id, .open = true, .pendingFocus = true });
	if (currentView) {
		const float cascade = Util::GetEditorChromeHeaderHeight();
		const ImVec2 sourcePos = ImGui::GetCurrentWindow()->RootWindow->Pos;
		view.spawnPos = ImVec2(sourcePos.x + cascade, sourcePos.y + cascade);
	}
}

void Widget::CloseFocusedView()
{
	const auto focused = std::ranges::find_if(views, [this](const WidgetView& view) { return view.open && view.id == focusedViewId; });
	(focused != views.end() ? *focused : views.front()).open = false;
}

bool Widget::BeginSettingsTabBar(const char* id)
{
	return Util::BeginPillTabBar(id, false, [this] {
		if (BrowserUI::IconButton("##AnotherView", Icons::TI(ICON_TI_LAYOUT_COLUMNS), GetAnotherViewTooltip()))
			OpenAnotherView();
	});
}

void Widget::DrawViews()
{
	for (auto& view : views) {
		if (!view.open)
			continue;
		currentView = &view;
		DrawWidget();
	}
	currentView = nullptr;
	views.remove_if([](const WidgetView& view) { return view.id != 0 && !view.open; });
}

bool Widget::BeginWidgetWindow(bool showApply, bool showSaveLoadRevert, bool showForceWeather, RE::TESWeather* weather, const char* searchId)
{
	auto& view = View();
	const std::string title = GetWindowTitle();
	SetupWidgetWindowDefaults(GetWidgetTypeName(), title.c_str());
	if (view.spawnPos) {
		ImGui::SetNextWindowPos(*view.spawnPos, ImGuiCond_Appearing);
		view.spawnPos.reset();
	}
	if (view.pendingFocus) {
		ImGui::SetNextWindowFocus();
		view.pendingFocus = false;
	}
	view.customHeaderActionsDrawn = false;

	// Weather type icon sits left of the title via drawLeading (standalone icon fonts).
	const auto typeIcon = WeatherTypeIcons::Resolve(weather);
	const bool hasLeadingIcon = typeIcon.has_value();

	bool result = Util::BeginWithCustomHeader(title.c_str(), &view.open, [this, &view, showApply, showSaveLoadRevert, showForceWeather, weather, searchId]() { view.customHeaderActionsDrawn = DrawTitleBarActions(showApply, showSaveLoadRevert, showForceWeather, weather, true, searchId); }, ImGuiWindowFlags_NoSavedSettings | kStickyHeaderFlags, hasLeadingIcon ? [typeIcon](ImVec2 iconMin, float iconSize) { WeatherTypeIcons::Draw(typeIcon, ImGui::GetWindowDrawList(), iconMin, iconSize,
																																																																																																						  ImGui::GetColorU32(ImGuiCol_Text)); } : std::function<void(ImVec2, float)>{});
	UpdateWidgetTypeSize(GetWidgetTypeName());
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
		focusedViewId = view.id;
	}
	return result;
}

void Widget::ForceWeatherReinit(RE::TESWeather* weather)
{
	auto* sky = globals::game::sky;
	if (weather && sky && sky->currentWeather == weather) {
		Util::ForceWeather(sky, weather, true);
		// An engaged lock owns the override slot; releasing it flickers the sky until the lock reasserts.
		if (EditorWindow::GetSingleton()->IsWeatherLocked())
			EditorWindow::MaintainWeatherLock();
		else
			sky->ReleaseWeatherOverride();
	}
}

void Widget::ForceCurrentWeatherReinit()
{
	if (auto* sky = globals::game::sky)
		ForceWeatherReinit(sky->currentWeather);
}

namespace
{
	struct TitleBarAction
	{
		enum class Kind
		{
			Icon,      ///< Texture icon (theme PNG)
			Glyph,     ///< Icon-font glyph
			Text,      ///< Framed text button
			LockBadge  ///< Pale green/red lock/unlock chip
		};

		std::string id;
		Kind kind = Kind::Text;
		ImTextureRef texture;
		Icons::GlyphRef glyph;  ///< Glyph kind
		std::string label;
		const char* tooltip = "";
		std::optional<ImVec4> textColor;  ///< Text/Glyph colour override
		std::optional<ImVec4> fillColor;  ///< Text kind: persistent fill colour override
		std::optional<ImVec4> hoverTint;  ///< Hover and press in this colour instead of the theme's (unsaved save, delete)
		bool locked = false;              ///< LockBadge kind
		bool enabled = true;
		std::function<void()> onClick;
		float width = 0.0f;
	};
}

bool Widget::DrawTitleBarActions(bool showApply, bool showSaveLoadRevert, bool showForceWeather, RE::TESWeather* weather, bool customHeaderRow, const char* searchId)
{
	auto* menu = globals::menu;
	auto* editorWindow = EditorWindow::GetSingleton();
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (!menu || !editorWindow || !window)
		return false;

	if (customHeaderRow && window->DockIsActive)
		return false;
	const bool inlineRow = !customHeaderRow;

	const auto& style = ImGui::GetStyle();
	const auto& statusPalette = menu->GetTheme().StatusPalette;
	const bool useIcons = !editorWindow->settings.useTextButtons;
	const float scale = Util::GetUIScale();
	using Kind = TitleBarAction::Kind;

	// Outlives `actions`, whose tooltips point into it.
	const auto packBadge = showSaveLoadRevert ? GetPackBadge() : std::nullopt;

	std::vector<TitleBarAction> actions;

	auto addIcon = [&](const char* id, ImTextureRef texture, const char* tooltip, std::function<void()> onClick) -> TitleBarAction& {
		TitleBarAction action;
		action.id = id;
		action.kind = Kind::Icon;
		action.texture = texture;
		action.tooltip = tooltip;
		action.onClick = std::move(onClick);
		return actions.emplace_back(std::move(action));
	};
	auto addGlyph = [&](const char* id, Icons::GlyphRef glyph, const char* tooltip, std::function<void()> onClick) -> TitleBarAction& {
		TitleBarAction action;
		action.id = id;
		action.kind = Kind::Glyph;
		action.glyph = glyph;
		action.tooltip = tooltip;
		action.onClick = std::move(onClick);
		return actions.emplace_back(std::move(action));
	};
	auto addText = [&](const char* id, const char* label, const char* tooltip, std::function<void()> onClick) -> TitleBarAction& {
		TitleBarAction action;
		action.id = id;
		action.kind = Kind::Text;
		action.label = label;
		action.tooltip = tooltip;
		action.onClick = std::move(onClick);
		return actions.emplace_back(std::move(action));
	};

	// Force Weather / Unlock: lock badge matching the floating action bar
	if (showForceWeather && weather) {
		const bool isLocked = editorWindow->IsWeatherLockedByUser(weather);
		TitleBarAction action;
		action.id = "##TitleForceWeather";
		action.kind = Kind::LockBadge;
		action.locked = isLocked;
		action.tooltip = EditorWindow::GetWeatherLockTooltip(isLocked);
		action.onClick = [editorWindow, weather]() { editorWindow->ToggleUserWeatherLock(weather); };
		actions.emplace_back(std::move(action));
	}

	// Apply
	if (showApply && (!editorWindow->settings.autoApplyChanges || RequiresManualApply())) {
		const char* tooltip = T(TKEY("apply_changes"), "Apply changes to the game");
		auto onClick = [this]() { ApplyChanges(); };
		if (useIcons && Util::IconLoader::GetIcons().applyToGame.texture) {
			addIcon("##TitleApply", Util::IconLoader::GetIcons().applyToGame.texture, tooltip, onClick);
		} else {
			auto& action = addText("##TitleApply", T(TKEY("apply"), "Apply"), tooltip, onClick);
			auto fill = statusPalette.SuccessColor;
			fill.w = ThemeManager::Constants::CSE_TITLE_APPLY_FILL_ALPHA;
			action.fillColor = fill;
		}
	}

	if (showSaveLoadRevert) {
		const bool hasSavedFile = HasSavedFile();
		const auto restoreAction = [&](const char* id, Icons::GlyphRef glyph, const char* label, const char* tooltip, SettingsSource source) {
			auto restore = [this, source] { RestoreSettings(source); };
			auto& action = useIcons ? addGlyph(id, glyph, tooltip, restore) : addText(id, label, tooltip, restore);
			action.enabled = source == SettingsSource::Saved ? hasSavedFile : HasSettingsSource(source);
		};
		const bool unsaved = HasUnsavedChanges();
		const char* saveTooltip = T(TKEY("save_record_tooltip"), "Save this record's settings.");
		auto saveClick = [this]() { Save(); };
		std::optional<ImVec4> unsavedColor;
		if (unsaved)
			unsavedColor = Util::Colors::GetUnsaved();
		if (useIcons) {
			auto& action = addGlyph("##TitleSave", Icons::TI(MenuIcons::kSave), saveTooltip, saveClick);
			action.textColor = unsavedColor;
			action.hoverTint = unsavedColor;
		} else {
			auto& action = addText("##TitleSave", T(TKEY("save"), "Save"), saveTooltip, saveClick);
			action.textColor = unsavedColor;
			action.hoverTint = unsavedColor;
		}
		restoreAction("##TitleSaved", SettingsActionIcons::kRestoreSaved, T("menu.restore_settings", "Restore Saved Settings"),
			T(TKEY("restore_saved_record_tooltip"), "Restore this record's values from your saved file."), SettingsSource::Saved);
		restoreAction("##TitlePreset", SettingsActionIcons::kReapplyOverride, T(TKEY("restore_preset"), "Restore Preset Values"),
			T(TKEY("restore_preset_tooltip"), "Restore this record's values from the enabled preset. Save to keep them in your file."), SettingsSource::Preset);
		restoreAction("##TitleDefaults", SettingsActionIcons::kRestoreDefaults, T(TKEY("revert_to_game_values"), "Revert to Record Values"),
			T(TKEY("revert_to_original"), "Revert to record values"), SettingsSource::Game);

		if (hasSavedFile) {
			const char* deleteTooltip = T(TKEY("delete_saved_file_tooltip"), "Delete saved file");
			auto deleteClick = []() { ImGui::OpenPopup("DeleteConfirmation"); };
			TitleBarAction* deleteAction = nullptr;
			if (useIcons) {
				deleteAction = &addGlyph("##TitleDelete", Icons::TI(ICON_TI_TRASH), deleteTooltip, deleteClick);
				deleteAction->textColor = statusPalette.Error;
			} else {
				deleteAction = &addText("##TitleDelete", T(TKEY("delete"), "Delete"), deleteTooltip, deleteClick);
				deleteAction->textColor = statusPalette.Error;
			}
			deleteAction->hoverTint = statusPalette.Error;
		}

		if (packBadge) {
			auto& action = addGlyph("##TitlePackSource", Icons::TI(MenuIcons::kPresets), packBadge->tooltip.c_str(), []() {});
			action.textColor = packBadge->color;
		}
	}

	if (searchId) {
		addGlyph("##TitleSearch", Icons::TI(MenuIcons::kSearch), T(TKEY("search_settings_hint"), "Search settings (Ctrl+F)"), [this] {
			View().requestSearch = true;
			View().searchDropdownAnchor = ImGui::GetItemRectMax();
		});
	}

	const float gap = style.ItemSpacing.x;
	const float fontSize = ImGui::GetFontSize();
	const float lockBadgeSize = Util::GetLockStatusBadgeSize();
	const size_t firstRightAction = !actions.empty() && actions.front().kind == Kind::LockBadge ? 1 : 0;
	// Match BeginWithCustomHeader's chrome height so actions share the title's midline.
	const float headerH = Util::GetEditorChromeHeaderHeight();
	const float buttonHeight = ImGui::GetFrameHeight();

	ImRect titleRect;
	float left = 0.0f;
	float right = 0.0f;

	if (!inlineRow) {
		// Floating custom header: SameLine after the title. Anchor the control band to the
		// full chrome row (not the short text item) so icons share equal top/bottom air.
		const ImRect titleItem(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const float rowTop = titleItem.Min.y - Util::GetEditorChromeTextCursorOffsetY(headerH);
		const float contentRight = window->WorkRect.Max.x;
		const float closeSize = fontSize + style.FramePadding.y * 2.0f;
		const float leftEdge = ImGui::GetCursorScreenPos().x;
		titleRect = ImRect(ImVec2(leftEdge, rowTop), ImVec2(contentRight, rowTop + headerH));
		left = leftEdge + (firstRightAction ? gap : 0.0f);
		right = contentRight - closeSize - gap;
	}

	const float titleLockX = left;
	if (firstRightAction) {
		actions.front().width = lockBadgeSize;
		left += lockBadgeSize + gap;
	}

	float actionsWidth = 0.0f;
	for (size_t i = firstRightAction; i < actions.size(); ++i) {
		auto& action = actions[i];
		if (action.kind == Kind::Text)
			action.width = ImGui::CalcTextSize(action.label.c_str()).x + style.FramePadding.x * 2.0f;
		else if (action.kind == Kind::LockBadge)
			action.width = lockBadgeSize;
		else
			action.width = buttonHeight;  // Icon / Glyph
		if (i > firstRightAction)
			actionsWidth += gap;
		actionsWidth += action.width;
	}

	const float cursorX = left;
	ImDrawList* drawList = window->DrawList;

	const float actionsLeft = right - actionsWidth;
	if (actions.empty())
		return true;  // Nothing to show; the title bar stays clean and no inline row is needed.

	if (!inlineRow && actionsLeft < cursorX)
		return false;  // Actions don't fit; caller draws them inline.

	// Draw the lock after the title and keep the remaining actions right-aligned.
	// Use InvisibleButton (same pattern as the main CS menu undocked header / close button) so hits
	// win over the custom-header drag catcher's AllowOverlap. ItemAdd+ButtonBehavior alone does not.
	float x = titleLockX;
	const ImVec4 iconTint = Util::GetIconTint();
	const float rounding = style.FrameRounding;

	std::function<void()> pendingClick;

	if (inlineRow)
		ImGui::BeginGroup();
	else
		ImGui::PushClipRect(titleRect.Min, titleRect.Max, false);
	for (size_t i = 0; i < actions.size(); ++i) {
		const auto& action = actions[i];
		if (i == firstRightAction)
			x = actionsLeft;
		else if (i > firstRightAction)
			x += gap;

		const float itemH = action.kind == Kind::LockBadge ? lockBadgeSize : buttonHeight;
		const float itemY = titleRect.Min.y + (titleRect.GetHeight() - itemH) * 0.5f;
		if (inlineRow && i > 0)
			Util::SameLineIfFits(action.width);
		const ImVec2 min = inlineRow ? ImGui::GetCursorScreenPos() : ImVec2(x, itemY);
		const ImRect bb(min, ImVec2(min.x + action.width, min.y + itemH));
		x += action.width;

		ImGui::SetCursorScreenPos(bb.Min);
		ImGui::BeginDisabled(!action.enabled);
		const bool clicked = ImGui::InvisibleButton(action.id.c_str(), bb.GetSize(), ImGuiButtonFlags_EnableNav);
		const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		if (hovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (clicked)
			pendingClick = action.onClick;

		// Hover and press ease in and out, in the action's state colour when it has one
		const auto DrawActionHighlight = [&] {
			auto tint = Util::StateHoverButtonStyle(action.hoverTint.value_or(ImVec4()), action.hoverTint.has_value());
			Util::DrawAnimatedButtonHighlight(bb, ImGui::GetItemID(), hovered, held, false, drawList);
		};

		switch (action.kind) {
		case Kind::LockBadge:
			Util::DrawLockStatusBadge(bb.Min, action.locked, drawList, hovered, held);
			break;
		case Kind::Icon:
			{
				DrawActionHighlight();
				const float iconInset = std::max(ThemeManager::Constants::CONTROL_OUTLINE_WIDTH * scale, buttonHeight * 0.1f);
				const ImVec2 iconMin = Util::IconLoader::SnapToPixel(ImVec2(bb.Min.x + iconInset, bb.Min.y + iconInset));
				const ImVec2 iconSize = Util::IconLoader::SnapToPixel(ImVec2(bb.GetWidth() - iconInset * 2.0f, bb.GetHeight() - iconInset * 2.0f));
				// The action holds the texture as an ImGui reference; its id is the icon's full view
				auto* fullView = reinterpret_cast<ID3D11ShaderResourceView*>(action.texture.GetTexID());
				drawList->AddImage(Util::IconLoader::ViewForSize(fullView, iconSize), iconMin, ImVec2(iconMin.x + iconSize.x, iconMin.y + iconSize.y),
					ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(iconTint));
				break;
			}
		case Kind::Glyph:
			{
				DrawActionHighlight();
				if (action.glyph) {
					const ImU32 col = action.textColor ? ImGui::GetColorU32(*action.textColor) : textCol;
					Icons::DrawCenteredGlyph(drawList, bb.Min, bb.GetSize(), action.glyph, col);
				}
				break;
			}
		case Kind::Text:
			{
				const auto blend = Util::Anim::ItemBlend(ImGui::GetItemID(), hovered, held, false);
				auto tint = Util::StateHoverButtonStyle(action.hoverTint.value_or(ImVec4()), action.hoverTint.has_value());
				ImU32 fill = ImGui::GetColorU32(ImGuiCol_Button);
				if (action.fillColor) {
					auto color = *action.fillColor;
					color.w = std::min(1.0f, color.w + WidgetUI::kTextActionHoverBoost * blend.Highlight());
					fill = ImGui::GetColorU32(color);
				} else {
					fill = Util::Anim::LerpU32(fill, ImGui::GetColorU32(ImGuiCol_ButtonHovered), blend.hover);
					fill = Util::Anim::LerpU32(fill, ImGui::GetColorU32(ImGuiCol_ButtonActive), blend.press);
				}
				drawList->AddRectFilled(bb.Min, bb.Max, fill, rounding);
				const ImVec2 textSize = ImGui::CalcTextSize(action.label.c_str());
				drawList->AddText(
					ImVec2(bb.Min.x + (action.width - textSize.x) * 0.5f, bb.Min.y + (itemH - textSize.y) * 0.5f),
					action.textColor ? ImGui::GetColorU32(*action.textColor) : textCol,
					action.label.c_str());
				break;
			}
		}

		Util::AddTooltip(action.tooltip, Util::kTooltipWhenDisabled);
		ImGui::EndDisabled();
	}
	if (inlineRow)
		ImGui::EndGroup();
	else
		ImGui::PopClipRect();

	if (pendingClick)
		pendingClick();

	return true;
}

void Widget::DrawWidgetHeader(const char* searchId, bool showApply, bool showSaveLoadRevert, bool showForceWeather, RE::TESWeather* weather)
{
	auto& view = View();
	if (view.navigatedFromSearch) {
		ClearSearchState(true);
		view.navigatedFromSearch = false;
	}
	const bool actionsInTitleBar = view.customHeaderActionsDrawn;
	if (!actionsInTitleBar)
		DrawTitleBarActions(showApply, showSaveLoadRevert, showForceWeather, weather, false, searchId);

	if (searchId && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
		view.requestSearch = true;
		view.searchDropdownAnchor = ImVec2(ImGui::GetCurrentWindow()->WorkRect.Max.x, ImGui::GetCursorScreenPos().y);
	}
	DrawDeleteConfirmationModal();

	const bool manualApply = showApply && RequiresManualApply() && EditorWindow::GetSingleton()->settings.autoApplyChanges;
	if (manualApply) {
		ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
		ImGui::TextWrapped("%s", T(TKEY("changes_require_manual_apply"), "(Changes require manual apply)"));
		ImGui::PopStyleColor();
		Util::AddTooltip(T(TKEY("manual_apply_required_tooltip"), "This form type is only re-read by the engine on weather reinit.\nAuto-apply is disabled - use the Apply button."));
	}
	if (!actionsInTitleBar || manualApply)
		ImGui::Separator();
}

void Widget::DrawSearchDropdown()
{
	auto& view = View();
	if (view.requestSearch) {
		ClearSearchState(true);
		ImGui::OpenPopup("##WidgetSettingsSearch");
		view.requestSearch = false;
	}
	const float width = WidgetUI::kSearchDropdownWidth * Util::GetUIScale();
	ImGui::SetNextWindowPos(view.searchDropdownAnchor, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Appearing);
	if (ImGui::BeginPopup("##WidgetSettingsSearch")) {
		view.searchPopupOpen = true;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			globals::menu->ConsumeEscape();
		const bool appearing = ImGui::IsWindowAppearing();
		if (appearing)
			ImGui::SetKeyboardFocusHere();
		Util::SearchField("##Query", view.searchBuffer, T(TKEY("search_settings_hint"), "Search settings (Ctrl+F)"), -FLT_MIN, false);
		if (appearing || view.searchResultsForQuery != view.searchBuffer) {
			view.searchResults = CollectSearchableSettings();
			std::erase_if(view.searchResults, [&](const SearchResult& result) {
				return !ContainsStringIgnoreCase(result.displayName, view.searchBuffer) &&
				       !ContainsStringIgnoreCase(result.tabName, view.searchBuffer) &&
				       !ContainsStringIgnoreCase(result.settingId, view.searchBuffer);
			});
			view.searchResultsForQuery = view.searchBuffer;
		}
		ImGui::Separator();
		const size_t visibleRows = std::max(size_t{ 1 }, std::min(WidgetUI::kSearchDropdownMaxResults, view.searchResults.size()));
		const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
		const float resultsHeight = rowHeight * static_cast<float>(visibleRows) + ImGui::GetStyle().WindowPadding.y * 2.0f;
		if (ImGui::BeginChild("##SearchDropdownResults", ImVec2(0.0f, resultsHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(view.searchResults.size()), rowHeight);
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const auto& result = view.searchResults[static_cast<size_t>(i)];
					const std::string label = result.tabName.empty() ? result.displayName : std::format("{} ({})", result.displayName, result.tabName);
					ImGui::PushID(i);
					if (Util::RoundedSelectable(label.c_str(), false)) {
						NavigateToSearchResult(result);
						view.navigatedFromSearch = true;
						ImGui::CloseCurrentPopup();
					}
					ImGui::PopID();
				}
			}
			if (view.searchResults.empty())
				ImGui::TextDisabled("%s", T("ui.no_results", "No results"));
		}
		ImGui::EndChild();
		ImGui::EndPopup();
	} else if (view.searchPopupOpen) {
		view.searchPopupOpen = false;
		ClearSearchState(true);
	}
}

void Widget::ClearSearchState(bool clearBuffer)
{
	auto& view = View();
	if (clearBuffer)
		view.searchBuffer.clear();
	view.searchResults.clear();
	view.searchResultsForQuery.clear();
}

void Widget::NavigateToSearchResult(const SearchResult& result)
{
	auto& view = View();
	view.activeTabOverride = result.tabName;
	view.highlightedSetting = result.settingId;
	view.highlightedDisplaySetting = result.displayName;
	view.highlightStartTime = static_cast<float>(ImGui::GetTime());
	view.scrollToHighlighted = true;
}

int Widget::GetTabFlagsForOverride(const std::string& tabName)
{
	auto& view = View();
	if (view.activeTabOverride.empty() || view.activeTabOverride != tabName)
		return 0;
	view.activeTabOverride.clear();
	return ImGuiTabItemFlags_SetSelected;
}

bool Widget::MatchesSearch(const std::string& settingId) const
{
	const auto& view = View();
	if (view.searchBuffer.empty())
		return true;
	return std::any_of(view.searchResults.begin(), view.searchResults.end(),
		[&](const SearchResult& r) { return r.settingId == settingId; });
}

void Widget::AutoApply(bool changed)
{
	if (changed && EditorWindow::GetSingleton()->settings.autoApplyChanges)
		ApplyChanges();
}

bool Widget::MatchesAnySearch(std::initializer_list<const char*> settingIds) const
{
	return std::any_of(settingIds.begin(), settingIds.end(), [&](const char* settingId) {
		return MatchesSearch(settingId);
	});
}

bool Widget::IsHighlighted(const std::string& settingId) const
{
	const auto& view = View();
	if (view.highlightedSetting != settingId && view.highlightedDisplaySetting != settingId)
		return false;
	const float elapsed = static_cast<float>(ImGui::GetTime()) - view.highlightStartTime;
	return elapsed < WidgetUI::kHighlightDurationSeconds;
}

void Widget::PushHighlightStyle(const std::string& settingId)
{
	if (!IsHighlighted(settingId))
		return;
	const float elapsed = static_cast<float>(ImGui::GetTime()) - View().highlightStartTime;
	const float normalized = std::clamp(elapsed / WidgetUI::kHighlightDurationSeconds, 0.0f, 1.0f);
	const float triangularFade = 1.0f - std::abs(normalized * 2.0f - 1.0f);
	const float alpha = std::clamp(WidgetUI::kHighlightMaxAlpha * triangularFade, 0.0f, WidgetUI::kHighlightMaxAlpha);
	ImVec4 frameBg = WidgetUI::kHighlightFrameBg;
	ImVec4 frameBgHovered = WidgetUI::kHighlightFrameBgHovered;
	frameBg.w = alpha;
	frameBgHovered.w = alpha;
	ImGui::PushStyleColor(ImGuiCol_FrameBg, frameBg);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameBgHovered);
}

void Widget::PopHighlightStyle(const std::string& settingId)
{
	if (!IsHighlighted(settingId))
		return;
	ImGui::PopStyleColor(2);
	if (auto& view = View(); view.scrollToHighlighted) {
		ImGui::SetScrollHereY(0.5f);
		view.scrollToHighlighted = false;
	}
}

#undef I18N_KEY_PREFIX
