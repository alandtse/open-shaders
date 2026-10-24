#include "FoliageLighting.h"

#include <algorithm>

#include "I18n/I18n.h"
#include "Util.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.foliage_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoliageLighting::Settings,
	Enable,
	EnableFoliageScattering,
	EnableFoliageAmbientBoost,
	EnableFoliageAmbientFlip,
	FoliageAmbientAmount,
	EnableGrassScattering);

void FoliageLighting::DrawSettings()
{
	Util::CheckboxFlag(T(TKEY("master_enabled"), "Enable Foliage Lighting"), settings.Enable);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("master_enabled_tip"), "Enables tree and grass scattering and tree ambient controls. All other settings are ignored when disabled."));
	ImGui::Indent();
	auto enabledGuard = Util::DisableGuard(!settings.Enable);

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
		ImGui::Text("%s", T(TKEY("enable_foliage_ambient_boost_tooltip"), "Adds the additive indirect ambient term to animated PBR foliage only."));
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
	ImGui::Unindent();
}

void FoliageLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FoliageLighting::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.FoliageAmbientAmount = std::clamp(settings.FoliageAmbientAmount, 0.0f, 1.0f);
}

void FoliageLighting::RestoreDefaultSettings()
{
	settings = {};
}

FoliageLighting::Settings FoliageLighting::GetCommonBufferData() const
{
	auto data = settings;
	if (!settings.Enable) {
		data.EnableFoliageScattering = 0;
		data.EnableFoliageAmbientBoost = 0;
		data.EnableFoliageAmbientFlip = 0;
		data.EnableGrassScattering = 0;
	}
	return data;
}

#undef I18N_KEY_PREFIX
