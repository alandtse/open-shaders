#include "SnowCover.h"

#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.snow_cover."

void SnowCover::DrawSettings()
{
	Util::CheckboxFlag(T(TKEY("master_enabled"), "Enable Snow Cover"), settings.Enable);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("master_enabled_tip"), "Enables seasonal snow, foliage tint, fire melting and Survival Mode cold. All other settings are ignored when disabled."));
	ImGui::Indent();
	auto enabledGuard = Util::DisableGuard(!settings.Enable);
	Util::CheckboxFlag(T(TKEY("nicer_foliage"), "Detailed foliage snow"), settings.EnableExpensiveFoliage);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("nicer_foliage_tip"), "Uses one extra texture sample to place snow along grass and tree billboard edges."));
	Util::CheckboxFlag(T(TKEY("mobile_objects"), "Snow on movable objects"), settings.AffectHavok);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("mobile_objects_tip"), "Includes movable clutter. World-space snow slides across tumbling objects. Actors are excluded."));
	Util::CheckboxFlag(T(TKEY("flora_tint"), "Tint harvestable plants"), settings.AffectFloraTint);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("flora_tint_tip"), "Applies seasonal foliage color to flowers and other harvestable plants. Snow still applies when this is off."));
	ImGui::SliderFloat(T(TKEY("snow_offset"), "Snow altitude offset"), &settings.SnowHeightOffset, DEFAULT_WINTER_HEIGHT_OFFSET, DEFAULT_SUMMER_HEIGHT_OFFSET);
	ImGui::SliderFloat(T(TKEY("rooftop_snow"), "Rooftop snow"), &settings.RooftopSnow, 0.0f, DEFAULT_SUMMER_HEIGHT_OFFSET, "%.0f");
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("rooftop_tip"), "Lowers the snow line on exposed roofs of buildings. Set to zero to disable."));
	{
		Util::DisableGuard disable(settings.RooftopSnow <= 0.0f);
		ImGui::SliderFloat(T(TKEY("rooftop_height"), "Rooftop height"), &settings.RooftopHeight, 32.0f, 1024.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("rooftop_exposure"), "Rooftop sky exposure"), &settings.RooftopExposure, 0.0f, 0.95f);
	}
	ImGui::Checkbox(T(TKEY("survival"), "Survival Mode cold"), &survivalEnabled);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("survival_tip"), "Snow-covered ground counts as freezing in Survival Mode and Survival Mode Improved."));
	ImGui::Checkbox(T(TKEY("fire_melt"), "Melt snow near fires"), &fireMeltSettings.Enabled);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("fire_melt_tip"), "Clears snow around up to eight nearby placed fires. Spells, projectiles and impacts are excluded."));
	if (fireMeltSettings.Enabled) {
		ImGui::SliderFloat(T(TKEY("fire_radius"), "Fire melt radius scale"), &fireMeltSettings.RadiusScale, 1.0f, 16.0f, "%.1fx");
		ImGui::SliderFloat(T(TKEY("fire_strength"), "Fire melt strength"), &fireMeltSettings.Strength, 0.0f, 1.0f);
	}
	ImGui::Separator();
	ImGui::TextUnformatted(lastWorldspace.c_str());
	ImGui::TextWrapped("%s", status.c_str());
	if (ImGui::Button(T(TKEY("reload"), "Reload config"))) {
		lastWorldspace.clear();
		LoadGeometryLists();
		UpdateFrame();
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("save"), "Save worldspace config")))
		SaveConfig();
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("defaults"), "Worldspace defaults"))) {
		const auto enabled = wsettings.EnableSnowCover;
		ResetWorldConfig();
		wsettings.EnableSnowCover = enabled;
	}
	if (ImGui::TreeNodeEx(T(TKEY("world_config"), "Worldspace config"), ImGuiTreeNodeFlags_DefaultOpen)) {
		DrawWorldSettings();
		ImGui::TreePop();
	}
	ImGui::Unindent();
	ValidateSettings();
}

void SnowCover::DrawWorldSettings()
{
	Util::CheckboxFlag(T(TKEY("enabled"), "Enable snow cover in this worldspace"), wsettings.EnableSnowCover);
	Util::CheckboxFlag(T(TKEY("grass_tint"), "Seasonal grass tint"), wsettings.AffectGrassTint);
	Util::CheckboxFlag(T(TKEY("tree_tint"), "Seasonal tree tint"), wsettings.AffectTreeTint);
	ImGui::DragFloat(T(TKEY("foliage_offset"), "Foliage color altitude offset"), &wsettings.FoliageHeightOffset, 10.0f, DEFAULT_WINTER_HEIGHT_OFFSET, DEFAULT_SUMMER_HEIGHT_OFFSET);
	ImGui::DragFloat(T(TKEY("blend"), "Snow transition width"), &wsettings.BlendSmoothness, 10.0f, 1.0f, 10000.0f);
	if (ImGui::TreeNodeEx(T(TKEY("seasons"), "Weather and seasons"))) {
		int summer = static_cast<int>(MaxSummerMonth);
		int winter = static_cast<int>(MaxWinterMonth);
		if (ImGui::SliderInt(T(TKEY("summer_month"), "Peak summer month (0-11)"), &summer, 0, 11))
			MaxSummerMonth = static_cast<uint>(summer);
		if (ImGui::SliderInt(T(TKEY("winter_month"), "Peak winter month (0-11)"), &winter, 0, 11))
			MaxWinterMonth = static_cast<uint>(winter);
		ImGui::DragFloat(T(TKEY("summer_height"), "Summer snow altitude"), &SummerHeightOffset, 10.0f, DEFAULT_WINTER_HEIGHT_OFFSET, DEFAULT_SUMMER_HEIGHT_OFFSET);
		ImGui::DragFloat(T(TKEY("winter_height"), "Winter snow altitude"), &WinterHeightOffset, 10.0f, DEFAULT_WINTER_HEIGHT_OFFSET, DEFAULT_SUMMER_HEIGHT_OFFSET);
		ImGui::SliderFloat(T(TKEY("snow_speed"), "Snow accumulation speed"), &snowingSpeed, 0.01f, 10.0f);
		ImGui::SliderFloat(T(TKEY("melt_speed"), "Snow melting speed"), &meltingSpeed, 0.01f, 10.0f);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("distance"), "Distance falloff"))) {
		ImGui::DragFloat(T(TKEY("weather_start"), "Weather snow fade start"), &wsettings.WeatherFadeStart, 100.0f, 0.0f, 200000.0f);
		ImGui::DragFloat(T(TKEY("weather_end"), "Weather snow fade end"), &wsettings.WeatherFadeEnd, 100.0f, 1.0f, 300000.0f);
		ImGui::DragFloat(T(TKEY("object_start"), "Object snow fade start"), &wsettings.ObjectFadeStart, 10.0f, 0.0f, 40000.0f);
		ImGui::DragFloat(T(TKEY("object_end"), "Object snow fade end"), &wsettings.ObjectFadeEnd, 10.0f, 1.0f, 40001.0f);
		ImGui::SliderFloat(T(TKEY("object_amount"), "Object snow fade amount"), &wsettings.ObjectFadeAmount, 0.0f, 1.0f);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("snow_map"), "Snow altitude map"))) {
		ImGui::InputText(T(TKEY("map_texture"), "Map texture"), mapTextureInput, sizeof(mapTextureInput));
		const bool firstChanged = ImGui::InputFloat2(T(TKEY("map_min"), "Map first corner"), &mapMin.x);
		const bool oppositeChanged = ImGui::InputFloat2(T(TKEY("map_max"), "Map opposite corner"), &mapMax.x);
		if (firstChanged || oppositeChanged)
			UpdateMapTransform();
		ImGui::DragFloat(T(TKEY("map_scale"), "Map altitude scale"), &wsettings.mapZscale, 100.0f, 0.1f, 1000000.0f);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("material"), "Snow material"))) {
		DrawMaterialSettings();
		ImGui::TreePop();
	}
}

void SnowCover::DrawMaterialSettings()
{
	ImGui::TextWrapped("%s", T(TKEY("texture_tip"), "Texture paths are relative to Data, without .dds. Materials use diffuse, _n and _rmaos textures. Save and reload to apply new paths."));
	ImGui::InputText(T(TKEY("main_texture"), "Main texture"), mainTextureInput, sizeof(mainTextureInput));
	ImGui::InputText(T(TKEY("alt_texture"), "Alternative texture"), alternateTextureInput, sizeof(alternateTextureInput));
	ImGui::ColorEdit4(T(TKEY("main_tint"), "Main tint"), &wsettings.MainTint.x);
	ImGui::ColorEdit4(T(TKEY("alt_tint"), "Alternative tint"), &wsettings.AltTint.x);
	ImGui::SliderFloat(T(TKEY("main_spec"), "Main specular"), &wsettings.MainSpec, 0.0f, 1.0f);
	ImGui::SliderFloat(T(TKEY("alt_spec"), "Alternative specular"), &wsettings.AltSpec, 0.0f, 1.0f);
	ImGui::TextWrapped("%s", T(TKEY("angle_tip"), "Slope controls use the upward normal: 0 is a vertical surface, 1 is horizontal. Tint alpha controls color coverage."));
	ImGui::SliderFloat(T(TKEY("min_angle"), "Minimum slope"), &wsettings.MinAngle, 0.0f, 1.0f);
	ImGui::SliderFloat(T(TKEY("max_angle"), "Full coverage slope"), &wsettings.MaxAngle, 0.0f, 1.0f);
	ImGui::SliderFloat(T(TKEY("peak_main"), "Main material peak slope"), &wsettings.PeakMainAngle, 0.0f, 1.0f);
	ImGui::SliderFloat(T(TKEY("peak_alt"), "Alternative material peak slope"), &wsettings.PeakAltAngle, 0.0f, 1.0f);
	ImGui::SliderFloat(T(TKEY("uv_scale"), "Texture scale"), &wsettings.UVScale, 0.01f, 10.0f);
	ImGui::SliderFloat(T(TKEY("glint_scale"), "Glint screen-space scale"), &wsettings.ScreenSpaceScale, 1.0f, 3.0f);
	ImGui::SliderFloat(T(TKEY("glint_density"), "Glint log microfacet density"), &wsettings.LogMicrofacetDensity, 0.0f, 40.0f);
	ImGui::SliderFloat(T(TKEY("glint_roughness"), "Glint microfacet roughness"), &wsettings.MicrofacetRoughness, 0.001f, 1.0f);
	ImGui::SliderFloat(T(TKEY("glint_random"), "Glint density variation"), &wsettings.DensityRandomization, 0.001f, 5.0f);
}
