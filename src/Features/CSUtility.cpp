#include "CSUtility.h"

#include "Bloom.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "LightLimitFix.h"
#include "LinearLighting.h"
#include "UnderwaterDepthOfField.h"
#include "Utils/PointLightFlags.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cmath>

#define I18N_KEY_PREFIX "feature.cs_utility."

namespace
{
	constexpr float kSkyBrightnessMin = 0.0f;
	constexpr float kSkyBrightnessMax = 2.0f;
	constexpr float kMultiplierMin = 0.0f;
	constexpr float kMultiplierMax = 5.0f;
	constexpr uint32_t kMaxVanillaPointLights = 7;
	constexpr uint32_t kVanillaPointLightCBRegister = 3;
	constexpr uint32_t kFirstPointLightSceneIndex = 1;

	float ClampFiniteOrDefault(float a_value, float a_min, float a_max, float a_default)
	{
		if (!std::isfinite(a_value))
			return a_default;
		return std::clamp(a_value, a_min, a_max);
	}

	void SanitizeSettings(CSUtility::Settings& a_settings)
	{
		const CSUtility::Settings defaults{};
		a_settings.skyBrightness = ClampFiniteOrDefault(a_settings.skyBrightness, kSkyBrightnessMin, kSkyBrightnessMax, defaults.skyBrightness);
		a_settings.directionalLightMult = ClampFiniteOrDefault(a_settings.directionalLightMult, kMultiplierMin, kMultiplierMax, defaults.directionalLightMult);
		a_settings.pointLightMult = ClampFiniteOrDefault(a_settings.pointLightMult, kMultiplierMin, kMultiplierMax, defaults.pointLightMult);
		a_settings.linearPointLightMult = ClampFiniteOrDefault(a_settings.linearPointLightMult, kMultiplierMin, kMultiplierMax, defaults.linearPointLightMult);
		a_settings.spotlightMult = ClampFiniteOrDefault(a_settings.spotlightMult, kMultiplierMin, kMultiplierMax, defaults.spotlightMult);
		a_settings.linearSpotlightMult = ClampFiniteOrDefault(a_settings.linearSpotlightMult, kMultiplierMin, kMultiplierMax, defaults.linearSpotlightMult);
		a_settings.omnidirectionalBulbMult = ClampFiniteOrDefault(a_settings.omnidirectionalBulbMult, kMultiplierMin, kMultiplierMax, defaults.omnidirectionalBulbMult);
		a_settings.linearOmnidirectionalBulbMult = ClampFiniteOrDefault(a_settings.linearOmnidirectionalBulbMult, kMultiplierMin, kMultiplierMax, defaults.linearOmnidirectionalBulbMult);
		CSUtility::SanitizeDepthOfFieldOverride(a_settings.sceneDof);
		CSUtility::SanitizeDepthOfFieldOverride(a_settings.underwaterDof);
		Bloom::SanitizeSettings(a_settings.bloomEnhancement);
	}

	void DrawMultiplierSlider(const char* a_label, float& a_value, float a_max = kMultiplierMax)
	{
		ImGui::SliderFloat(a_label, &a_value, kMultiplierMin, a_max, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	}

	void DrawLinearMultiplierSlider(const char* a_label, float& a_value, bool a_linearLightingEnabled)
	{
		ImGui::BeginDisabled(!a_linearLightingEnabled);
		DrawMultiplierSlider(a_label, a_value);
		ImGui::EndDisabled();

		if (!a_linearLightingEnabled) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("linear_slider_disabled_tooltip"), "Enable Linear Lighting to use this multiplier."));
			}
		}
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CSUtility::DepthOfFieldAutoFocusSettings,
	nearDistance,
	farDistance,
	nearRange,
	farRange,
	nearBlur,
	farBlur,
	blurMultiplier)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CSUtility::DepthOfFieldSettings,
	strength,
	distance,
	range,
	mode,
	excludeSky,
	autoFocus,
	autoFocusSettings,
	blurRadius)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CSUtility::DepthOfFieldOverride,
	locked,
	values,
	baseline)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CSUtility::Settings,
	skyBrightness,
	directionalLightMult,
	pointLightMult,
	linearPointLightMult,
	spotlightMult,
	linearSpotlightMult,
	omnidirectionalBulbMult,
	linearOmnidirectionalBulbMult,
	sceneDof,
	underwaterDof,
	bloomEnhancement)

void CSUtility::DrawSettings()
{
	if (ImGui::BeginTabBar("##CSUtilityTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_atmosphere"), "Atmosphere"))) {
			ImGui::SliderFloat(T(TKEY("sky_brightness"), "Sky Brightness"), &settings.skyBrightness, kSkyBrightnessMin, kSkyBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_multipliers"), "Multipliers"))) {
			if (ImGui::TreeNodeEx(T(TKEY("lighting"), "Lighting"), ImGuiTreeNodeFlags_DefaultOpen)) {
				const bool linearLightingEnabled = globals::features::linearLighting.settings.enableLinearLighting;
				DrawMultiplierSlider(T(TKEY("global_point_lighting"), "Global Point Lighting"), settings.pointLightMult);
				DrawLinearMultiplierSlider(T(TKEY("global_point_lighting_linear"), "Global Point Lighting (Linear)"), settings.linearPointLightMult, linearLightingEnabled);
				DrawMultiplierSlider(T(TKEY("spotlights"), "Spotlights"), settings.spotlightMult);
				DrawLinearMultiplierSlider(T(TKEY("spotlights_linear"), "Spotlights (Linear)"), settings.linearSpotlightMult, linearLightingEnabled);
				DrawMultiplierSlider(T(TKEY("omnidirectional_bulbs"), "Omnidirectional Bulbs"), settings.omnidirectionalBulbMult);
				DrawLinearMultiplierSlider(T(TKEY("omnidirectional_bulbs_linear"), "Omnidirectional Bulbs (Linear)"), settings.linearOmnidirectionalBulbMult, linearLightingEnabled);
				DrawMultiplierSlider(T(TKEY("directional_light_multiplier"), "Directional Light Multiplier"), settings.directionalLightMult);
				ImGui::TreePop();
			}
			ImGui::EndTabItem();
		}

		DrawDepthOfFieldSettings();
		DrawVanillaBloomSettings();

		ImGui::EndTabBar();
	}
}

void CSUtility::DrawVanillaBloomSettings()
{
	if (ImGui::BeginTabItem(T(TKEY("tab_vanilla_bloom"), "Vanilla Bloom"))) {
		Bloom::DrawSettings(settings.bloomEnhancement);
		ImGui::EndTabItem();
	}
}

void CSUtility::LoadSettings(json& o_json)
{
	settings = o_json;
	SanitizeSettings(settings);
}

void CSUtility::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
}

void CSUtility::RestoreDefaultSettings()
{
	settings = {};
}

void CSUtility::SetupResources()
{
	vanillaPointLightCB = new ConstantBuffer(ConstantBufferDesc<VanillaPointLightData>(), "OSUtility::VanillaPointLightData");
}

CSUtility::PerFrameData CSUtility::GetCommonBufferData() const
{
	Settings sanitizedSettings = settings;
	SanitizeSettings(sanitizedSettings);

	PerFrameData data{};
	data.skyBrightness = sanitizedSettings.skyBrightness;
	data.directionalLightMult = sanitizedSettings.directionalLightMult;
	data.pointLightMult = sanitizedSettings.pointLightMult;
	data.linearPointLightMult = sanitizedSettings.linearPointLightMult;
	data.spotlightMult = sanitizedSettings.spotlightMult;
	data.linearSpotlightMult = sanitizedSettings.linearSpotlightMult;
	data.omnidirectionalBulbMult = sanitizedSettings.omnidirectionalBulbMult;
	data.linearOmnidirectionalBulbMult = sanitizedSettings.linearOmnidirectionalBulbMult;
	return data;
}

void CSUtility::UpdateVanillaPointLightData(RE::BSRenderPass* a_pass, uint32_t a_lightCount)
{
	if (!vanillaPointLightCB || !a_pass || !a_pass->sceneLights)
		return;

	VanillaPointLightData data{};
	const uint32_t lightCount = std::min(a_lightCount, kMaxVanillaPointLights);
	for (uint32_t lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
		const uint32_t sceneLightIndex = lightIndex + kFirstPointLightSceneIndex;
		if (sceneLightIndex >= a_pass->numLights)
			break;

		auto* bsLight = a_pass->sceneLights[sceneLightIndex];
		if (!bsLight)
			continue;

		auto* niLight = bsLight->light.get();
		data.pointLightFlags[lightIndex] = PointLightFlags::GetVanillaPointLightFlags(bsLight, niLight);
	}

	vanillaPointLightCB->Update(data);

	ID3D11Buffer* buffer = vanillaPointLightCB->CB();
	globals::d3d::context->PSSetConstantBuffers(kVanillaPointLightCBRegister, 1, &buffer);
}

struct CSUtility::Hooks
{
	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);

			auto& csUtility = globals::features::csUtility;
			if (!csUtility.loaded || globals::features::lightLimitFix.loaded)
				return;

			const uint32_t lightCount = a_pass && a_pass->numLights > 0 ? a_pass->numLights - kFirstPointLightSceneIndex : 0;
			csUtility.UpdateVanillaPointLightData(a_pass, lightCount);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
		logger::info("[CSUtility] Installed hooks");
	}
};

void CSUtility::PostPostLoad()
{
	Hooks::Install();
	InstallDepthOfFieldHooks();
}

void CSUtility::DataLoaded()
{
	UnderwaterDepthOfField::InstallHooks();
}

#undef I18N_KEY_PREFIX
