#include "FoliageLighting.h"

#include <algorithm>

#include "I18n/I18n.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.foliage_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoliageLighting::Settings,
	EnableFoliageScattering,
	EnableFoliageAmbientBoost,
	EnableFoliageAmbientFlip,
	FoliageAmbientAmount,
	EnableGrassScattering);

void FoliageLighting::DrawSettings()
{
	bool enableFoliageScattering = settings.EnableFoliageScattering != 0;
	if (ImGui::Checkbox(T(TKEY("enable_foliage_scattering"), "Tree Foliage Scattering"), &enableFoliageScattering)) {
		settings.EnableFoliageScattering = enableFoliageScattering;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_foliage_scattering_tooltip"), "Adds wrapped, view-dependent transmission to animated tree foliage."));
	}

	bool enableGrassScattering = settings.EnableGrassScattering != 0;
	if (ImGui::Checkbox(T(TKEY("enable_grass_scattering"), "Grass Scattering"), &enableGrassScattering)) {
		settings.EnableGrassScattering = enableGrassScattering;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_grass_scattering_tooltip"), "Adds wrapped, view-dependent transmission to non-PBR grass lighting."));
	}

	bool enableFoliageAmbientBoost = settings.EnableFoliageAmbientBoost != 0;
	if (ImGui::Checkbox(T(TKEY("enable_foliage_ambient_boost"), "Tree Ambient Boost"), &enableFoliageAmbientBoost)) {
		settings.EnableFoliageAmbientBoost = enableFoliageAmbientBoost;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_foliage_ambient_boost_tooltip"), "Adds the legacy additive indirect ambient term to animated trees only."));
	}

	bool enableFoliageAmbientFlip = settings.EnableFoliageAmbientFlip != 0;
	if (ImGui::Checkbox(T(TKEY("enable_foliage_ambient_flip"), "Tree Ambient Backface Flip"), &enableFoliageAmbientFlip)) {
		settings.EnableFoliageAmbientFlip = enableFoliageAmbientFlip;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_foliage_ambient_flip_tooltip"), "Mirrors the ambient sampling normal for visible backside tree foliage cards."));
	}

	ImGui::SliderFloat(T(TKEY("foliage_ambient_amount"), "Tree Ambient Amount"), &settings.FoliageAmbientAmount, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("foliage_ambient_amount_tooltip"), "Amount added to the diffuse indirect ambient response for animated trees."));
	}
}

void FoliageLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FoliageLighting::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.FoliageAmbientAmount = std::clamp(settings.FoliageAmbientAmount, 0.0f, 1.0f);
	legacySettingsMigrated = false;
}

void FoliageLighting::RestoreDefaultSettings()
{
	if (legacySettingsMigrated) {
		legacySettingsMigrated = false;
		return;
	}
	settings = {};
}

void FoliageLighting::MigrateLegacySettings(const json& a_json)
{
	settings.EnableFoliageScattering = a_json.value("EnableFoliageScattering", settings.EnableFoliageScattering);
	settings.EnableFoliageAmbientBoost = a_json.value("EnableFoliageAmbientBoost", settings.EnableFoliageAmbientBoost);
	settings.EnableFoliageAmbientFlip = a_json.value("EnableFoliageAmbientFlip", settings.EnableFoliageAmbientFlip);
	settings.FoliageAmbientAmount = std::clamp(a_json.value("FoliageAmbientAmount", settings.FoliageAmbientAmount), 0.0f, 1.0f);
	settings.EnableGrassScattering = a_json.value("EnableGrassScattering", settings.EnableGrassScattering);
	legacySettingsMigrated = true;
}

#undef I18N_KEY_PREFIX
