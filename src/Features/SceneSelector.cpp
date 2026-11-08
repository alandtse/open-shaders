#include "SceneSelector.h"
#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.weather_picker."

#include "Feature.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/Game.h"
#include "Utils/StringUtils.h"
#include "Utils/UI.h"

#include "CSEditor.h"
#include "CSEditor/Browser/BrowserWidgets.h"
#include "CSEditor/EditorWindow.h"
#include "CSEditor/WeatherPickerWindow.h"
#include "Features/PerformanceOverlay.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <nlohmann/json.hpp>

namespace
{
	struct WeatherFlagInfo
	{
		RE::TESWeather::WeatherDataFlag flag;
		std::string_view canonicalName;
	};

	constexpr std::array<WeatherFlagInfo, 6> kClassifiedWeatherFlags = { {
		{ RE::TESWeather::WeatherDataFlag::kPleasant, "Pleasant" },
		{ RE::TESWeather::WeatherDataFlag::kCloudy, "Cloudy" },
		{ RE::TESWeather::WeatherDataFlag::kRainy, "Rainy" },
		{ RE::TESWeather::WeatherDataFlag::kSnow, "Snow" },
		{ RE::TESWeather::WeatherDataFlag::kPermAurora, "Aurora" },
		{ RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun, "Aurora Sun" },
	} };

	const WeatherFlagInfo* FindWeatherFlagInfo(RE::TESWeather::WeatherDataFlag flag)
	{
		auto it = std::ranges::find(kClassifiedWeatherFlags, flag, &WeatherFlagInfo::flag);
		return it != kClassifiedWeatherFlags.end() ? &*it : nullptr;
	}

	const WeatherFlagInfo* FindWeatherFlagInfo(std::string_view canonicalName)
	{
		auto it = std::ranges::find(kClassifiedWeatherFlags, canonicalName, &WeatherFlagInfo::canonicalName);
		return it != kClassifiedWeatherFlags.end() ? &*it : nullptr;
	}

	bool DrawAnalysisSectionHeader(const char* label)
	{
		constexpr auto flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		return ImGui::TreeNodeEx(label, flags);
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SceneSelector::WeatherDetailsWindowSettings,
	Enabled,
	ShowInOverlay,
	Position,
	PositionSet)

void SceneSelector::LoadSettings(json& o_json)
{
	WeatherDetailsWindow = o_json;
}

void SceneSelector::SaveSettings(json& o_json)
{
	o_json = WeatherDetailsWindow;
	o_json["ShowInOverlay"] = IsOverlayVisible();
}

void SceneSelector::PostPostLoad()
{
	// Before the game loop starts, so no call site can be executing while it is rewritten.
	EditorWindow::InstallWeatherLockHooks();
}

void SceneSelector::DataLoaded()
{
	s_destinations.clear();
	const auto append = [](RE::TESForm* form) {
		if (!form || !Util::SceneTravel::GetCommand(form))
			return;
		s_destinations.push_back({ form, Util::GetSpatialFormDisplayName(form),
			Util::GetFormEditorID(form), form->GetFormType() });
	};
	if (auto* data = RE::TESDataHandler::GetSingleton()) {
		for (auto* location : data->GetFormArray<RE::BGSLocation>())
			append(location);
		for (auto* cell : Util::GetLoadedCells())
			append(cell);
		for (auto* worldspace : data->GetFormArray<RE::TESWorldSpace>())
			append(worldspace);
	}
	std::ranges::sort(s_destinations, {}, &TravelDestination::label);
}

void SceneSelector::Prepass()
{
	if (!globals::features::csEditor.loaded)
		EditorWindow::MaintainWeatherLock();
}

void SceneSelector::DrawSettings()
{
	if (ImGui::BeginTable("##SceneSelectorControls", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("##Weather", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("##Time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableNextColumn();
		if (auto* sky = globals::game::sky)
			RenderWeatherControls(sky);
		ImGui::TableNextColumn();
		ImGui::SeparatorText(T(TKEY("time_control"), "Time Control"));
		EditorWindow::GetSingleton()->DrawTimeControls(true);
		DrawTeleportControls();
		ImGui::EndTable();
	}
	ImGui::Spacing();
	DrawShowInOverlayToggle();
	if (auto* sky = globals::game::sky) {
		ImGui::SeparatorText(T(TKEY("analysis"), "Analysis"));
		RenderWeatherInformationDisplay(sky);
		RenderFeatureWeatherAnalysis();
	}
}

void SceneSelector::DrawShowInOverlayToggle()
{
	const auto& themeSettings = Menu::GetSingleton()->GetTheme();
	const auto& menuSettings = Menu::GetSingleton()->GetSettings();

	bool showInOverlay = IsOverlayVisible();
	if (ImGui::Checkbox(T(TKEY("show_in_overlay"), "Show in Overlay"), &showInOverlay)) {
		WeatherDetailsWindow.ShowInOverlay = showInOverlay;
		globals::features::performanceOverlay.settings.ShowWeatherPicker = false;
		WeatherDetailsWindow.Enabled = showInOverlay;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("show_in_overlay_tooltip"),
							  "Opens weather details in a separate window that stays open\neven when the main menu is closed. "));
		ImGui::TextUnformatted(T(TKEY("toggle_with"), "Toggle with "));
		ImGui::SameLine();
		ImGui::TextColored(themeSettings.StatusPalette.CurrentHotkey, "%s", Util::Input::KeyIdToString(menuSettings.OverlayToggleKey).c_str());
	}
}

// ================================================================================
// Scene Selector functionality
// ================================================================================

void SceneSelector::ResetWindowLayout()
{
	WeatherDetailsWindow.PositionSet = false;
	resetWindowSize = true;
}

void SceneSelector::OpenWeatherOverlay()
{
	if (EditorWindow::GetSingleton()->open) {
		WeatherPickerWindow::GetSingleton()->open = true;
		return;
	}
	WeatherDetailsWindow.ShowInOverlay = true;
	WeatherDetailsWindow.Enabled = true;
	globals::features::performanceOverlay.settings.ShowWeatherPicker = false;
	Menu::GetSingleton()->overlayVisible = true;
}

void SceneSelector::RenderWeatherDetailsWindow(bool* open)
{
	if (!open || !*open)
		return;

	auto player = globals::game::player;
	if (!player || !player->parentCell)
		return;

	const bool allowInputs =
		Menu::GetSingleton()->ShouldSwallowInput() ||
		(globals::game::ui && globals::game::ui->IsMenuOpen(RE::CursorMenu::MENU_NAME));

	// Set initial position if not already set
	const float scale = Util::GetUIScale();
	if (!WeatherDetailsWindow.PositionSet) {
		const auto* viewport = ImGui::GetMainViewport();
		const float padding = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;
		const ImVec2 defaultPosition(
			viewport->WorkPos.x + viewport->WorkSize.x - padding,
			viewport->WorkPos.y + padding);
		ImGui::SetNextWindowPos(defaultPosition, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		WeatherDetailsWindow.Position = defaultPosition;
		WeatherDetailsWindow.PositionSet = true;
	} else {
		ImGui::SetNextWindowPos(WeatherDetailsWindow.Position, ImGuiCond_FirstUseEver);
	}

	ImGui::SetNextWindowSize(
		ImVec2(600 * scale, 800 * scale),
		resetWindowSize ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
	const ImGuiWindowFlags windowFlags = allowInputs ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs;
	if (Util::BeginWithRoundedClose("Weather Details##Popup", open, windowFlags)) {
		// Remember window position for next frame
		ImVec2 currentPos = ImGui::GetWindowPos();
		if (currentPos.x != WeatherDetailsWindow.Position.x || currentPos.y != WeatherDetailsWindow.Position.y) {
			WeatherDetailsWindow.Position = currentPos;
		}

		// Keep scroll state separate when the weather controls appear or disappear.
		const char* contentId = allowInputs ? "##InteractiveWeatherDetails" : "##ReadOnlyWeatherDetails";
		if (ImGui::BeginChild(contentId, { 0, 0 })) {
			RenderCoreWeatherDetails(allowInputs);
			RenderFeatureWeatherAnalysis();
		}
		ImGui::EndChild();
	}
	ImGui::End();
	resetWindowSize = false;
}

ImVec4 SceneSelector::GetWeatherTypeColor(RE::TESWeather* weather)
{
	if (!weather) {
		return Menu::GetSingleton()->GetTheme().StatusPalette.InfoColor;
	}

	const auto& theme = Menu::GetSingleton()->GetTheme();

	// Priority order for weather classification colors (highest priority first)
	static const std::vector<RE::TESWeather::WeatherDataFlag> priorityOrder = {
		RE::TESWeather::WeatherDataFlag::kRainy,
		RE::TESWeather::WeatherDataFlag::kSnow,
		RE::TESWeather::WeatherDataFlag::kPermAurora,
		RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun,
		RE::TESWeather::WeatherDataFlag::kCloudy,
		RE::TESWeather::WeatherDataFlag::kPleasant
	};

	// Check flags in priority order
	for (const auto& flag : priorityOrder) {
		if (weather->data.flags.any(flag)) {
			return GetWeatherFlagColor(flag);
		}
	}

	// Check for unclassified/unflagged weather
	if (weather->data.flags.underlying() == 0) {
		return Menu::GetSingleton()->GetTheme().StatusPalette.Warning;
	}

	return theme.StatusPalette.InfoColor;  // Default blue
}

// --- Helper: Display basic weather info (name, flags, percentage) ---
void SceneSelector::DisplayWeatherBasicInfo(RE::TESWeather* weather, float weatherPct)
{
	if (!weather) {
		ImGui::BulletText("%s", T(TKEY("no_weather_found"), "No Weather Found"));
		return;
	}
	std::string weatherText = Util::FormatWeather(weather);
	ImGui::Bullet();
	ImGui::SameLine();
	bool showTooltip = SceneSelector::RenderMultiColorWeatherName(weather, weatherText);
	if (showTooltip) {
		ImGui::BeginTooltip();
		ImGui::Text(T(TKEY("tooltip_name"), "Name: %s"), weather->GetName() ? weather->GetName() : T(TKEY("unnamed"), "Unnamed"));
		ImGui::Text(T(TKEY("tooltip_editor_id_2"), "Editor ID: %s"), weather->GetFormEditorID() ? weather->GetFormEditorID() : T(TKEY("none_value"), "None"));
		ImGui::Text(T(TKEY("tooltip_form_id_2"), "Form ID: 0x%08X"), weather->GetFormID());
		auto flagNames = SceneSelector::GetWeatherFlagNames(weather);
		if (!flagNames.empty()) {
			std::string joinedFlags = flagNames[0];
			for (size_t j = 1; j < flagNames.size(); ++j) {
				joinedFlags += ", " + flagNames[j];
			}
			ImGui::Text(T(TKEY("tooltip_flags"), "Flags: %s"), joinedFlags.c_str());
		} else {
			ImGui::Text("%s", T(TKEY("tooltip_flags_none"), "Flags: None"));
		}
		ImGui::EndTooltip();
	}
	if (weatherPct >= 0.0f) {
		ImGui::BulletText(T(TKEY("weather_percentage"), "Weather Percentage: %.1f%%"), weatherPct * 100.0f);
	}
}

void SceneSelector::DisplayPrecipitationInfo(RE::TESWeather* weather)
{
	if (!weather || !weather->precipitationData) {
		ImGui::BulletText("%s", T(TKEY("no_precipitation_data"), "Particle Density: No precipitation data"));
		return;
	}
	auto particleDensity = weather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
	ImGui::BulletText(T(TKEY("particle_density"), "Particle Density: %.3f"), particleDensity);
	GET_INSTANCE_MEMBER(particleTexture, weather->precipitationData)
	if (!particleTexture.textureName.empty()) {
		ImGui::BulletText(T(TKEY("particle_texture"), "Particle Texture: %s"), particleTexture.textureName.c_str());
	} else {
		ImGui::BulletText("%s", T(TKEY("particle_texture_none"), "Particle Texture: None"));
	}
	uint8_t precipBeginFadeIn = weather->data.precipitationBeginFadeIn;
	uint8_t precipEndFadeOut = weather->data.precipitationEndFadeOut;
	float precipBeginNormalized = precipBeginFadeIn / 255.0f;
	float precipEndNormalized = precipEndFadeOut / 255.0f;
	ImGui::BulletText(T(TKEY("precip_begin_fade_in"), "Precip Begin Fade-In: %.3f (raw %u)"), precipBeginNormalized, precipBeginFadeIn);
	ImGui::BulletText(T(TKEY("precip_end_fade_out"), "Precip End Fade-Out: %.3f (raw %u)"), precipEndNormalized, precipEndFadeOut);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ T(TKEY("precip_fade_info_0"), "Precipitation fade transition parameters:"),
			T(TKEY("precip_fade_info_1"), "Begin Fade-In: Point where precipitation starts appearing"),
			T(TKEY("precip_fade_info_2"), "End Fade-Out: Point where precipitation fully disappears"),
			T(TKEY("precip_fade_info_3"), "Raw values: 0-255 (uint8), Normalized: 0.0-1.0") });
	}
}

void SceneSelector::DisplayLightningInfo(RE::TESWeather* weather, bool showInteractiveElements)
{
	if (!weather || (uint8_t)weather->data.thunderLightningFrequency == 0)
		return;
	const auto& theme = Menu::GetSingleton()->GetTheme();
	uint8_t lightningR = weather->data.lightningColor.red;
	uint8_t lightningG = weather->data.lightningColor.green;
	uint8_t lightningB = weather->data.lightningColor.blue;
	ImGui::Text("%s", T(TKEY("lightning_color"), "Lightning Color:"));
	ImGui::SameLine();
	float lightningColor[3] = { lightningR / 255.0f, lightningG / 255.0f, lightningB / 255.0f };
	ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
	if (!showInteractiveElements) {
		flags |= ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, theme.StatusPalette.Disable.w);
	}
	bool colorChanged = ImGui::ColorEdit3("##LightningColor", lightningColor, flags);
	if (!showInteractiveElements) {
		ImGui::PopStyleVar();
	}
	if (colorChanged && showInteractiveElements) {
		weather->data.lightningColor.red = static_cast<std::uint8_t>(lightningColor[0] * 255.0f + 0.5f);
		weather->data.lightningColor.green = static_cast<std::uint8_t>(lightningColor[1] * 255.0f + 0.5f);
		weather->data.lightningColor.blue = static_cast<std::uint8_t>(lightningColor[2] * 255.0f + 0.5f);
	}
	uint8_t thunderFreqRaw = (uint8_t)weather->data.thunderLightningFrequency;
	ImGui::BulletText(T(TKEY("thunder_frequency"), "Thunder Frequency: %u"), static_cast<unsigned>(thunderFreqRaw));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ T(TKEY("thunder_freq_info_0"), "Thunder frequency raw value (0-255):"),
			"",
			T(TKEY("thunder_freq_info_1"), "Known data points from Creation Kit slider:"),
			T(TKEY("thunder_freq_info_2"), "- Raw 15 = ~100% frequency (highest thunder)"),
			T(TKEY("thunder_freq_info_3"), "- Raw 76 = ~75% frequency"),
			T(TKEY("thunder_freq_info_4"), "- Raw 203 = ~20% frequency"),
			T(TKEY("thunder_freq_info_5"), "- Raw 246 = ~5% frequency"),
			T(TKEY("thunder_freq_info_6"), "- Raw 255 = ~0% frequency (lowest thunder)"),
			"",
			T(TKEY("thunder_freq_info_7"), "Range: 0-255 (unsigned 8-bit integer)"),
			T(TKEY("thunder_freq_info_8"), "Note: Creation Kit interprets this value non-linearly") });
	}
	uint8_t lightningBeginFadeIn = weather->data.thunderLightningBeginFadeIn;
	uint8_t lightningEndFadeOut = weather->data.thunderLightningEndFadeOut;
	float lightningBeginNormalized = lightningBeginFadeIn / 255.0f;
	float lightningEndNormalized = lightningEndFadeOut / 255.0f;
	ImGui::BulletText(T(TKEY("lightning_begin_fade_in"), "Lightning Begin Fade-In: %.3f (raw %u)"), lightningBeginNormalized, lightningBeginFadeIn);
	ImGui::BulletText(T(TKEY("lightning_end_fade_out"), "Lightning End Fade-Out: %.3f (raw %u)"), lightningEndNormalized, lightningEndFadeOut);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ T(TKEY("lightning_fade_info_0"), "Lightning fade transition parameters:"),
			T(TKEY("lightning_fade_info_1"), "Begin Fade-In: Point where lightning starts appearing"),
			T(TKEY("lightning_fade_info_2"), "End Fade-Out: Point where lightning fully disappears"),
			T(TKEY("lightning_fade_info_3"), "Raw values: 0-255 (uint8), Normalized: 0.0-1.0") });
	}
}

void SceneSelector::DisplayWindInfo(RE::TESWeather* weather)
{
	auto sky = globals::game::sky;
	if (!weather || (weather->data.windSpeed <= 0 && (!sky || sky->windSpeed <= 0.0f)))
		return;
	float windSpeedDisplay = weather->data.windSpeed / 255.0f;
	ImGui::BulletText(T(TKEY("weather_wind_speed"), "Weather Wind Speed: %.2f (raw %d)"), windSpeedDisplay, weather->data.windSpeed);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::string windStr = Util::Units::FormatWindSpeed(weather->data.windSpeed);
		Util::DrawMultiLineTooltip({ T(TKEY("wind_speed_tooltip_0"), "Wind speed from weather definition"),
			windStr.c_str() });
	}
	if (sky) {
		ImGui::BulletText(T(TKEY("sky_wind_speed"), "Sky Wind Speed: %.2f"), sky->windSpeed);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			Util::DrawMultiLineTooltip({ T(TKEY("sky_wind_tooltip_0"), "Current active wind speed from the sky system"),
				T(TKEY("sky_wind_tooltip_1"), "This affects particle behavior and wind-based effects") });
		}
	}
	float weatherWindDirDegrees = Util::Units::DirectionRawToDegrees(weather->data.windDirection);
	ImGui::BulletText(T(TKEY("wind_direction"), "Wind Direction: %.1f\xc2\xb0 (raw %d)"), weatherWindDirDegrees, weather->data.windDirection);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::string dirStr = Util::Units::FormatDirection(weather->data.windDirection);
		Util::DrawMultiLineTooltip({ T(TKEY("wind_direction_tooltip_0"), "Wind direction from weather definition"),
			dirStr.c_str() });
	}
	float weatherWindRangeDegrees = Util::Units::DirectionRangeToDegrees(weather->data.windDirectionRange);
	ImGui::BulletText(T(TKEY("wind_direction_range"), "Wind Direction Range: %.1f\xc2\xb0 (raw %d)"), weatherWindRangeDegrees, weather->data.windDirectionRange);

	if (auto player = globals::game::player) {
		float playerAngleZ = player->GetAngleZ();
		float playerAngleDegrees = Util::Units::NormalizeDegrees0To360(Util::Units::RadiansToDegrees(playerAngleZ));
		ImGui::BulletText(T(TKEY("player_direction"), "Player Direction: %.1f\xc2\xb0"), playerAngleDegrees);
		float effectiveWindDirection = Util::Units::NormalizeDegrees0To360(weatherWindDirDegrees - WIND_DIRECTION_OFFSET);
		float rawDifference = Util::Units::NormalizeDegreesToSignedRange(effectiveWindDirection - playerAngleDegrees);
		ImGui::BulletText(T(TKEY("effective_wind_dir"), "Effective Wind Dir: %.1f\xc2\xb0 (raw - %.1f\xc2\xb0)"), effectiveWindDirection, WIND_DIRECTION_OFFSET);
		ImGui::BulletText(T(TKEY("wind_vs_player"), "Wind vs Player: %.1f\xc2\xb0"), rawDifference);
		const char* windRelation;
		if (std::abs(rawDifference) < 30.0f) {
			windRelation = T(TKEY("tailwind"), "Tailwind (wind behind player)");
		} else if (std::abs(rawDifference) > 150.0f) {
			windRelation = T(TKEY("headwind"), "Headwind (wind coming toward player)");
		} else if (rawDifference > 0) {
			windRelation = T(TKEY("right_crosswind"), "Right crosswind");
		} else {
			windRelation = T(TKEY("left_crosswind"), "Left crosswind");
		}
		ImGui::SameLine();
		Util::Text::RestartNeeded("(%s)", windRelation);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			Util::DrawMultiLineTooltip({
				T(TKEY("wind_vs_player_tooltip_0"), "Wind relative to player direction:"),
				T(TKEY("wind_vs_player_tooltip_1"), "- ~0\xc2\xb0 = Tailwind (wind behind player)"),
				T(TKEY("wind_vs_player_tooltip_2"),
					"- ~\xc2\xb1"
					"90\xc2\xb0 = Crosswind (left/right)"),
				T(TKEY("wind_vs_player_tooltip_3"),
					"- ~\xc2\xb1"
					"180\xc2\xb0 = Headwind (wind coming toward player)"),
			});
		}
	}
}

// --- Main function: now just delegates to helpers ---
void SceneSelector::DisplayWeatherInfo(RE::TESWeather* weather, float weatherPct, bool showInteractiveElements)
{
	ImGui::SeparatorText(T(TKEY("overview"), "Overview"));
	SceneSelector::DisplayWeatherBasicInfo(weather, weatherPct);
	if (!weather)
		return;

	ImGui::SeparatorText(T(TKEY("precipitation"), "Precipitation"));
	SceneSelector::DisplayPrecipitationInfo(weather);

	if ((uint8_t)weather->data.thunderLightningFrequency != 0) {
		ImGui::SeparatorText(T(TKEY("lightning"), "Lightning"));
		SceneSelector::DisplayLightningInfo(weather, showInteractiveElements);
	}

	auto sky = globals::game::sky;
	if (weather->data.windSpeed > 0 || (sky && sky->windSpeed > 0.0f)) {
		ImGui::SeparatorText(T(TKEY("wind"), "Wind"));
		SceneSelector::DisplayWindInfo(weather);
	}
}

void SceneSelector::RenderWeatherControls(RE::Sky* sky)
{
	ImGui::SeparatorText(T(TKEY("weather_selection"), "Weather Selection"));
	const Util::DisableGuard disabled(!sky || sky->mode.get() != RE::Sky::Mode::kFull);
	WeatherPickerWindow::GetSingleton()->DrawContents(true);
}

void SceneSelector::DrawTeleportControls()
{
	ImGui::SeparatorText(T(TKEY("teleport"), "Teleport"));
	static RE::TESForm* selected = nullptr;
	static bool locations = false;
	static bool cells = false;
	static bool worldspaces = false;
	const auto selectedEntry = std::ranges::find(s_destinations, selected, &TravelDestination::form);
	const auto* preview = selectedEntry == s_destinations.end() ? T(TKEY("select_destination"), "Select a destination...") : selectedEntry->label.c_str();
	const auto& style = ImGui::GetStyle();
	const auto* go = T(TKEY("teleport"), "Teleport");
	const float buttonWidth = ImGui::CalcTextSize(go).x + style.FramePadding.x * 2.0f;
	ImGui::SetNextItemWidth(std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().x - buttonWidth - style.ItemSpacing.x));
	const float listHeight = ImGui::GetTextLineHeightWithSpacing() * ThemeManager::Constants::CSE_PICKER_LIST_ROWS;
	const float popupHeight = listHeight + ImGui::GetFrameHeightWithSpacing() * 2.0f + style.ItemSpacing.y * 2.0f + style.WindowPadding.y * 2.0f;
	const auto* viewport = ImGui::GetWindowViewport();
	const float availablePopupHeight = viewport->WorkSize.y - style.DisplaySafeAreaPadding.y * 2.0f;
	const float height = std::min(popupHeight, availablePopupHeight);
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, height), ImVec2(FLT_MAX, height));
	if (ImGui::BeginCombo("##TeleportDestination", preview, ImGuiComboFlags_HeightLargest)) {
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto search = Util::DrawComboSearchInput("TeleportDestination");
		if (BrowserUI::FilterChip("##Locations", { T(TKEY("locations"), "Locations") }, locations))
			locations = !locations;
		const auto* cellsLabel = T(TKEY("cells"), "Cells");
		BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(cellsLabel, false));
		if (BrowserUI::FilterChip("##Cells", { cellsLabel }, cells))
			cells = !cells;
		const auto* worldspacesLabel = T(TKEY("worldspaces"), "Worldspaces");
		BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(worldspacesLabel, false));
		if (BrowserUI::FilterChip("##Worldspaces", { worldspacesLabel }, worldspaces))
			worldspaces = !worldspaces;
		static std::vector<const TravelDestination*> matches;
		matches.clear();
		for (const auto& destination : s_destinations) {
			if ((locations || cells || worldspaces) &&
				!((destination.type == RE::FormType::Location && locations) ||
					(destination.type == RE::FormType::Cell && cells) ||
					(destination.type == RE::FormType::WorldSpace && worldspaces)))
				continue;
			if (!search.empty() && !Util::StringMatchesSearch(destination.label, search) && !Util::StringMatchesSearch(destination.editorId, search))
				continue;
			matches.push_back(&destination);
		}
		const float availableHeight = ImGui::GetContentRegionAvail().y;
		if (ImGui::BeginChild("##TeleportDestinations", ImVec2(0.0f, std::min(listHeight, std::max(ImGui::GetTextLineHeightWithSpacing(), availableHeight))), ImGuiChildFlags_AlwaysUseWindowPadding)) {
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(matches.size()));
			while (clipper.Step()) {
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
					const auto& destination = *matches[index];
					ImGui::PushID(static_cast<int>(destination.form->GetFormID()));
					if (Util::RoundedSelectable(destination.label.c_str(), selected == destination.form)) {
						selected = destination.form;
						ImGui::CloseCurrentPopup();
					}
					ImGui::PopID();
					if (auto tooltip = Util::HoverTooltipWrapper())
						ImGui::TextUnformatted(destination.editorId.c_str());
				}
			}
		}
		ImGui::EndChild();
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	const Util::DisableGuard disabled(!selected || !Util::SceneTravel::GetCommand(selected) || !globals::game::player || !globals::game::player->parentCell ||
									  !globals::state || globals::state->IsMainOrLoadingMenuOpen());
	if (Util::Button(go))
		Util::SceneTravel::Go(selected);
}

void SceneSelector::RenderWeatherInformationDisplay(RE::Sky* sky, bool showInteractiveElements)
{
	// Update cache: store current lastWeather if it exists, otherwise keep the cached one
	if (sky->lastWeather) {
		s_cachedLastWeather = sky->lastWeather;
	}

	if (!DrawAnalysisSectionHeader(T(TKEY("current_last_weather_analysis"), "Current & Last Weather Analysis")))
		return;

	// Use cached last weather for display if sky->lastWeather is null
	RE::TESWeather* displayLastWeather = sky->lastWeather ? sky->lastWeather : s_cachedLastWeather;

	// Create resizable 2-column table for current and last weather
	if (ImGui::BeginTable("WeatherComparison", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
		// Set up columns
		ImGui::TableSetupColumn(T(TKEY("current_weather_column"), "Current Weather"), ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn(T(TKEY("last_weather_column"), "Last Weather"), ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableHeadersRow();

		ImGui::TableNextRow();

		// Current Weather Column
		ImGui::TableNextColumn();
		ImGui::PushID("CurrentWeather");
		DisplayWeatherInfo(sky->currentWeather, sky->currentWeatherPct, showInteractiveElements);
		ImGui::PopID();

		// Last Weather Column
		ImGui::TableNextColumn();
		ImGui::PushID("LastWeather");
		DisplayWeatherInfo(displayLastWeather, std::abs(sky->currentWeatherPct - 1.0f), false);
		ImGui::PopID();

		ImGui::EndTable();
	}
}

void SceneSelector::RenderCoreWeatherDetails(bool showInteractiveElements)
{
	const auto showError = [](const char* msg) {
		auto menu = Menu::GetSingleton();
		const auto& theme = menu->GetTheme();
		ImGui::TextColored(theme.StatusPalette.Error, "%s", msg);
	};

	if (auto sky = globals::game::sky) {
		if (showInteractiveElements) {
			RenderWeatherControls(sky);
		}
		if (sky->mode.get() == RE::Sky::Mode::kFull) {
			ImGui::Spacing();
			ImGui::SeparatorText(T(TKEY("analysis"), "Analysis"));
			RenderWeatherInformationDisplay(sky, showInteractiveElements);
			ImGui::Spacing();
		} else {
			showError(T(TKEY("sky_not_full"), "Sky not in full mode"));
		}
	} else {
		showError(T(TKEY("sky_not_available"), "Sky not available"));
	}
}

void SceneSelector::RenderFeatureWeatherAnalysis()
{
	auto sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull)
		return;

	// Iterate through all loaded features to show their weather analysis
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			// Skip SceneSelector itself to avoid recursion
			if (feature == &globals::features::sceneSelector) {
				continue;
			}

			// Check if this feature provides weather analysis
			auto weatherConfig = feature->GetWeatherAnalysisConfig();
			if (weatherConfig.sectionName.empty()) {
				continue;  // Skip features that don't provide weather analysis
			}

			auto featureName = feature->GetShortName();
			ImGui::PushID(featureName.c_str());

			bool isExpanded = DrawAnalysisSectionHeader(weatherConfig.sectionName.c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(T(TKEY("feature_weather_analysis_tooltip"), "%s weather analysis"), feature->GetDisplayName().c_str());
			}

			if (isExpanded) {
				if (weatherConfig.drawFunction) {
					weatherConfig.drawFunction();
				}
			}

			ImGui::PopID();
		}
	}
}

std::vector<std::string> SceneSelector::GetWeatherFlagNames(RE::TESWeather* weather)
{
	std::vector<std::string> flagNames;
	if (!weather) {
		return flagNames;
	}

	uint32_t flags = weather->data.flags.underlying();
	if (flags == 0) {
		flagNames.push_back("None");
		return flagNames;
	}

	// Use magic_enum to iterate through all weather flags
	for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
		if (flagValue != RE::TESWeather::WeatherDataFlag::kNone &&
			weather->data.flags.any(flagValue)) {
			if (const auto* info = FindWeatherFlagInfo(flagValue)) {
				flagNames.emplace_back(info->canonicalName);
			} else {
				std::string flagName = std::string(magic_enum::enum_name(flagValue));
				flagNames.push_back(flagName.starts_with("k") ? flagName.substr(1) : flagName);
			}
		}
	}

	// Check for any unknown flags (flags not covered by the enum)
	uint32_t knownFlags = 0;
	for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
		if (flagValue != RE::TESWeather::WeatherDataFlag::kNone) {
			knownFlags |= static_cast<uint32_t>(flagValue);
		}
	}

	uint32_t unknownFlags = flags & ~knownFlags;
	if (unknownFlags != 0) {
		flagNames.push_back(std::format("{}({})", T(TKEY("unknown"), "Unknown"), unknownFlags));
	}

	return flagNames;
}

bool SceneSelector::RenderMultiColorWeatherName(RE::TESWeather* weather, const std::string& weatherName)
{
	if (!weather) {
		ImGui::Text("%s", weatherName.c_str());
		return false;
	}

	// Get all flags present in this weather
	std::vector<std::string> flagNames = GetWeatherFlagNames(weather);

	// If no flags or only one flag, use simple single-color display
	if (flagNames.size() <= 1) {
		ImVec4 weatherColor = GetWeatherTypeColor(weather);
		ImGui::PushStyleColor(ImGuiCol_Text, weatherColor);
		ImGui::Text("%s", weatherName.c_str());
		ImGui::PopStyleColor();
		return ImGui::IsItemHovered();
	}
	// For multiple flags, create a color-coded display
	// We'll show the weather name in segments, each with its own color

	// Create a visual representation with colored segments
	// Format: "WeatherName [Flag1][Flag2][Flag3]"

	// Display the main weather name in the primary color (highest priority flag)
	ImVec4 primaryColor = GetWeatherTypeColor(weather);
	ImGui::PushStyleColor(ImGuiCol_Text, primaryColor);

	// Extract base weather name (without the flag suffix)
	std::string baseName = weatherName;
	size_t bracketPos = baseName.find(" [");
	if (bracketPos != std::string::npos) {
		baseName = baseName.substr(0, bracketPos);
	}

	ImGui::Text("%s", baseName.c_str());
	ImGui::PopStyleColor();

	// Check if the main weather name (the most important part) was hovered
	bool baseNameHovered = ImGui::IsItemHovered();

	// Display flags as colored chips on the same line
	ImGui::SameLine();
	ImGui::TextUnformatted(" ");

	for (size_t i = 0; i < flagNames.size(); ++i) {
		if (flagNames[i] == "None" || flagNames[i].find("Unknown") == 0) {
			continue;  // Skip "None" and "Unknown" flags for cleaner display
		}

		ImGui::SameLine();
		ImVec4 flagColor = GetWeatherFlagColorByName(flagNames[i]);
		ImGui::PushStyleColor(ImGuiCol_Text, flagColor);
		// Translate canonical flag name for display
		const std::string flagKey = Util::ToLowerAscii(std::string(TKEY("flag_")) + flagNames[i]);
		const char* displayFlag = T(flagKey.c_str(), flagNames[i].c_str());
		ImGui::Text("[%s]", displayFlag);
		ImGui::PopStyleColor();
	}

	// Return true if the base name (largest, most visible part) was hovered
	return baseNameHovered;
}

// Helper function to get color for a specific weather flag
ImVec4 SceneSelector::GetWeatherFlagColor(RE::TESWeather::WeatherDataFlag flag)
{
	const auto& theme = Menu::GetSingleton()->GetTheme();

	switch (flag) {
	case RE::TESWeather::WeatherDataFlag::kRainy:
		return ImVec4(0.4f, 0.7f, 1.0f, 1.0f);  // Light blue for rain
	case RE::TESWeather::WeatherDataFlag::kSnow:
		return ImVec4(0.9f, 0.9f, 1.0f, 1.0f);  // Light blue-white for snow
	case RE::TESWeather::WeatherDataFlag::kPermAurora:
		return ImVec4(0.8f, 0.4f, 1.0f, 1.0f);  // Purple for aurora
	case RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun:
		return ImVec4(0.9f, 0.6f, 1.0f, 1.0f);  // Light purple for aurora follows sun
	case RE::TESWeather::WeatherDataFlag::kCloudy:
		return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray for cloudy
	case RE::TESWeather::WeatherDataFlag::kPleasant:
		return theme.StatusPalette.SuccessColor;  // Green for pleasant
	default:
		return theme.StatusPalette.InfoColor;  // Default blue
	}
}

// Helper function to get color for a specific flag name
ImVec4 SceneSelector::GetWeatherFlagColorByName(const std::string& flagName)
{
	if (const auto* info = FindWeatherFlagInfo(flagName)) {
		return GetWeatherFlagColor(info->flag);
	}

	// Default for unclassified or unknown flags
	return Menu::GetSingleton()->GetTheme().StatusPalette.Warning;
}

#undef I18N_KEY_PREFIX

void SceneSelector::DrawOverlay()
{
	auto player = globals::game::player;
	if (!player || !player->parentCell)
		return;

	const bool overlayVisible = !EditorWindow::GetSingleton()->open && Menu::GetSingleton()->overlayVisible && IsOverlayVisible();
	static bool s_prevOverlayVisible = false;
	// If ShowInOverlay is true and overlay is visible, auto-enable the window if not already enabled
	if (overlayVisible) {
		if (!s_prevOverlayVisible && !WeatherDetailsWindow.Enabled) {
			WeatherDetailsWindow.Enabled = true;
		}
		bool* p_open = &WeatherDetailsWindow.Enabled;
		RenderWeatherDetailsWindow(p_open);
	}
	s_prevOverlayVisible = overlayVisible;
}

bool SceneSelector::IsOverlayVisible() const
{
	return WeatherDetailsWindow.ShowInOverlay || globals::features::performanceOverlay.settings.ShowWeatherPicker;
}
