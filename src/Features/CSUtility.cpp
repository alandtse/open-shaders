#include "CSUtility.h"

#include "Bloom.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "LightLimitFix.h"
#include "LinearLighting.h"
#include "UnderwaterDepthOfField.h"
#include "Utils/MathUtils.h"
#include "Utils/PointLightFlags.h"
#include "Utils/UI.h"
#include "VolumetricLighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#define I18N_KEY_PREFIX "feature.cs_utility."

namespace
{
	constexpr float kSkyBrightnessMin = 0.0f;
	constexpr float kSkyBrightnessMax = 2.0f;
	constexpr float kWeatherBrightnessMin = 0.0f;
	constexpr float kWeatherBrightnessMax = 2.0f;
	constexpr float kSkySaturationMin = 0.0f;
	constexpr float kSkySaturationMax = 2.0f;
	constexpr float kSkyStaticTransparencyMin = 0.0f;
	constexpr float kSkyStaticTransparencyMax = 1.0f;
	constexpr float kMultiplierMin = 0.0f;
	constexpr float kMultiplierMax = 5.0f;
	constexpr float kSceneBrightnessMin = 0.25f;
	constexpr float kSceneBrightnessMax = 2.0f;
	constexpr float kGammaOffsetMin = -1.0f;
	constexpr float kGammaOffsetMax = 1.0f;
	constexpr float kSceneAmbientWeight = 0.95f;
	constexpr float kSceneDirectionalWeight = 0.70f;
	constexpr float kScenePointWeight = 0.75f;
	constexpr float kSceneEmissiveWeight = 0.35f;
	constexpr float kSceneEffectWeight = 0.55f;
	constexpr float kSceneGammaWeight = 0.35f;
	constexpr float kSceneSkyGammaWeight = 0.90f;
	constexpr float kSceneFogGammaWeight = 0.75f;
	constexpr float kSceneFogAlphaGammaWeight = 0.50f;
	constexpr float kSceneWaterGammaWeight = 0.75f;
	constexpr float kSceneVolumetricGammaWeight = 0.85f;
	constexpr float kWaterBrightnessMin = 0.0f;
	constexpr float kWaterBrightnessMax = 2.0f;
	constexpr float kWaterAmountMin = 0.0f;
	constexpr float kWaterAmountMax = 2.0f;
	constexpr float kWaterSunSpecularMax = 5.0f;
	constexpr float kWaterFresnelMin = 0.0f;
	constexpr float kWaterFresnelMax = 1.0f;
	constexpr float kWaterCausticsTilingMin = 0.25f;
	constexpr float kWaterCausticsTilingMax = 4.0f;
	constexpr float kWaterCausticsSpeedMax = 3.0f;
	constexpr int kWaterParallaxQualityMin = 4;
	constexpr int kWaterParallaxQualityMax = 64;
	constexpr uint32_t kMaxVanillaPointLights = 7;
	constexpr uint32_t kVanillaPointLightCBRegister = 3;
	constexpr uint32_t kFirstPointLightSceneIndex = 1;

	void SanitizeSettings(CSUtility::Settings& a_settings)
	{
		const CSUtility::Settings defaults{};
		a_settings.skyBrightness = Util::ClampFiniteOrDefault(a_settings.skyBrightness, kSkyBrightnessMin, kSkyBrightnessMax, defaults.skyBrightness);
		a_settings.cloudBrightness = Util::ClampFiniteOrDefault(a_settings.cloudBrightness, kSkyBrightnessMin, kSkyBrightnessMax, defaults.cloudBrightness);
		a_settings.skySaturation = Util::ClampFiniteOrDefault(a_settings.skySaturation, kSkySaturationMin, kSkySaturationMax, defaults.skySaturation);
		a_settings.cloudSaturation = Util::ClampFiniteOrDefault(a_settings.cloudSaturation, kSkySaturationMin, kSkySaturationMax, defaults.cloudSaturation);
		a_settings.ambientLightMult = Util::ClampFiniteOrDefault(a_settings.ambientLightMult, kMultiplierMin, kMultiplierMax, defaults.ambientLightMult);
		a_settings.directionalLightMult = Util::ClampFiniteOrDefault(a_settings.directionalLightMult, kMultiplierMin, kMultiplierMax, defaults.directionalLightMult);
		a_settings.pointLightMult = Util::ClampFiniteOrDefault(a_settings.pointLightMult, kMultiplierMin, kMultiplierMax, defaults.pointLightMult);
		a_settings.linearPointLightMult = Util::ClampFiniteOrDefault(a_settings.linearPointLightMult, kMultiplierMin, kMultiplierMax, defaults.linearPointLightMult);
		a_settings.spotlightMult = Util::ClampFiniteOrDefault(a_settings.spotlightMult, kMultiplierMin, kMultiplierMax, defaults.spotlightMult);
		a_settings.linearSpotlightMult = Util::ClampFiniteOrDefault(a_settings.linearSpotlightMult, kMultiplierMin, kMultiplierMax, defaults.linearSpotlightMult);
		a_settings.omnidirectionalBulbMult = Util::ClampFiniteOrDefault(a_settings.omnidirectionalBulbMult, kMultiplierMin, kMultiplierMax, defaults.omnidirectionalBulbMult);
		a_settings.linearOmnidirectionalBulbMult = Util::ClampFiniteOrDefault(a_settings.linearOmnidirectionalBulbMult, kMultiplierMin, kMultiplierMax, defaults.linearOmnidirectionalBulbMult);
		a_settings.sceneBrightness = Util::ClampFiniteOrDefault(a_settings.sceneBrightness, kSceneBrightnessMin, kSceneBrightnessMax, defaults.sceneBrightness);
		a_settings.emitColorMult = Util::ClampFiniteOrDefault(a_settings.emitColorMult, kMultiplierMin, kMultiplierMax, defaults.emitColorMult);
		a_settings.glowmapMult = Util::ClampFiniteOrDefault(a_settings.glowmapMult, kMultiplierMin, kMultiplierMax, defaults.glowmapMult);
		a_settings.effectLightingMult = Util::ClampFiniteOrDefault(a_settings.effectLightingMult, kMultiplierMin, kMultiplierMax, defaults.effectLightingMult);
		a_settings.skyGammaOffset = Util::ClampFiniteOrDefault(a_settings.skyGammaOffset, kGammaOffsetMin, kGammaOffsetMax, defaults.skyGammaOffset);
		a_settings.cloudGammaOffset = Util::ClampFiniteOrDefault(a_settings.cloudGammaOffset, kGammaOffsetMin, kGammaOffsetMax, defaults.cloudGammaOffset);
		a_settings.effectBrightness = Util::ClampFiniteOrDefault(a_settings.effectBrightness, kWeatherBrightnessMin, kWeatherBrightnessMax, defaults.effectBrightness);
		a_settings.skyStaticBrightness = Util::ClampFiniteOrDefault(a_settings.skyStaticBrightness, kWeatherBrightnessMin, kWeatherBrightnessMax, defaults.skyStaticBrightness);
		a_settings.skyStaticTransparency = Util::ClampFiniteOrDefault(a_settings.skyStaticTransparency, kSkyStaticTransparencyMin, kSkyStaticTransparencyMax, defaults.skyStaticTransparency);
		a_settings.fogGammaOffset = Util::ClampFiniteOrDefault(a_settings.fogGammaOffset, kGammaOffsetMin, kGammaOffsetMax, defaults.fogGammaOffset);
		a_settings.fogAlphaGammaOffset = Util::ClampFiniteOrDefault(a_settings.fogAlphaGammaOffset, kGammaOffsetMin, kGammaOffsetMax, defaults.fogAlphaGammaOffset);
		a_settings.fogIntensity = Util::ClampFiniteOrDefault(a_settings.fogIntensity, kMultiplierMin, kMultiplierMax, defaults.fogIntensity);
		a_settings.waterGammaOffset = Util::ClampFiniteOrDefault(a_settings.waterGammaOffset, kGammaOffsetMin, kGammaOffsetMax, defaults.waterGammaOffset);
		a_settings.vlGammaOffset = Util::ClampFiniteOrDefault(a_settings.vlGammaOffset, kGammaOffsetMin, kGammaOffsetMax, defaults.vlGammaOffset);
		a_settings.vlIntensity = Util::ClampFiniteOrDefault(a_settings.vlIntensity, kMultiplierMin, kMultiplierMax, defaults.vlIntensity);
		a_settings.sunGlareIntensity = Util::ClampFiniteOrDefault(a_settings.sunGlareIntensity, kMultiplierMin, kMultiplierMax, defaults.sunGlareIntensity);
		CSUtility::SanitizeWaterSettings(a_settings.water);
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

	void DrawGammaOffsetSlider(const char* a_label, float& a_value)
	{
		ImGui::SliderFloat(a_label, &a_value, kGammaOffsetMin, kGammaOffsetMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextWrapped("%s", T(TKEY("gamma_offset_tooltip"), "Adjusts the brightness curve. Negative values lift midtones; positive values lower them. Zero preserves the current curve."));
		}
	}

	void DrawWaterSlider(const char* a_label, float& a_value, float a_min, float a_max, const char* a_tooltip)
	{
		ImGui::SliderFloat(a_label, &a_value, a_min, a_max, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextWrapped("%s", a_tooltip);
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
	CSUtility::WaterSettings,
	brightness,
	reflectionAmount,
	refractionAmount,
	sunSpecularMultiplier,
	waveAmplitude,
	fresnelMin,
	fresnelMax,
	muddiness,
	causticsStrength,
	causticsTiling,
	causticsSpeed,
	causticsDispersion,
	parallaxStrength,
	parallaxQuality)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CSUtility::Settings,
	skyBrightness,
	cloudBrightness,
	skySaturation,
	cloudSaturation,
	ambientLightMult,
	directionalLightMult,
	pointLightMult,
	linearPointLightMult,
	spotlightMult,
	linearSpotlightMult,
	omnidirectionalBulbMult,
	linearOmnidirectionalBulbMult,
	sceneBrightness,
	emitColorMult,
	glowmapMult,
	effectLightingMult,
	skyGammaOffset,
	cloudGammaOffset,
	effectBrightness,
	skyStaticBrightness,
	skyStaticTransparency,
	fogGammaOffset,
	fogAlphaGammaOffset,
	fogIntensity,
	waterGammaOffset,
	vlGammaOffset,
	vlIntensity,
	sunGlareIntensity,
	water,
	sceneDof,
	underwaterDof,
	bloomEnhancement)

void CSUtility::DrawSettings()
{
	if (ImGui::BeginTabBar("##CSUtilityTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_atmosphere"), "Atmosphere"))) {
			activeSettingsPage = SettingsPage::Atmosphere;
			ImGui::SliderFloat(T(TKEY("sky_brightness"), "Sky Brightness"), &settings.skyBrightness, kSkyBrightnessMin, kSkyBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("cloud_brightness"), "Cloud Brightness"), &settings.cloudBrightness, kSkyBrightnessMin, kSkyBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("sky_saturation"), "Sky Saturation"), &settings.skySaturation, kSkySaturationMin, kSkySaturationMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextWrapped("%s", T(TKEY("sky_saturation_tooltip"), "Zero makes the sky grayscale. One preserves the current colors; higher values increase saturation."));
			}
			ImGui::SliderFloat(T(TKEY("cloud_saturation"), "Cloud Saturation"), &settings.cloudSaturation, kSkySaturationMin, kSkySaturationMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			DrawGammaOffsetSlider(T(TKEY("sky_gamma_offset"), "Sky Gamma Offset"), settings.skyGammaOffset);
			DrawGammaOffsetSlider(T(TKEY("cloud_gamma_offset"), "Cloud Gamma Offset"), settings.cloudGammaOffset);
			ImGui::SliderFloat(T(TKEY("effect_brightness"), "Effect Brightness"), &settings.effectBrightness, kWeatherBrightnessMin, kWeatherBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextWrapped("%s", T(TKEY("effect_brightness_tooltip"), "Scales the current weather's Effect Lighting color. One preserves your weather edits; lower values darken it and higher values brighten it."));
			}
			ImGui::SliderFloat(T(TKEY("sky_static_brightness"), "Sky Static Brightness"), &settings.skyStaticBrightness, kWeatherBrightnessMin, kWeatherBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextWrapped("%s", T(TKEY("sky_static_brightness_tooltip"), "Scales the current weather's Sky Statics color. One preserves your weather edits; lower values darken it and higher values brighten it."));
			}
			ImGui::SliderFloat(T(TKEY("sky_static_transparency"), "Sky Static Transparency"), &settings.skyStaticTransparency, kSkyStaticTransparencyMin, kSkyStaticTransparencyMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextWrapped("%s", T(TKEY("sky_static_transparency_tooltip"), "Fades sky static meshes. Zero preserves their current visibility; one makes them fully transparent."));
			}
			DrawMultiplierSlider(T(TKEY("fog_intensity"), "Vanilla Fog Intensity"), settings.fogIntensity);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextWrapped("%s", T(TKEY("fog_intensity_tooltip"), "Scales vanilla distance fog opacity. Zero removes it; one preserves its current strength. Has no effect when Exponential Height Fog disables vanilla fog."));
			}
			DrawGammaOffsetSlider(T(TKEY("fog_gamma_offset"), "Vanilla Fog Gamma Offset"), settings.fogGammaOffset);
			ImGui::SliderFloat(T(TKEY("fog_transparency_gamma_offset"), "Vanilla Fog Transparency Gamma Offset"), &settings.fogAlphaGammaOffset, kGammaOffsetMin, kGammaOffsetMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextWrapped("%s", T(TKEY("fog_transparency_gamma_offset_tooltip"), "Adjusts vanilla distance fog opacity. Negative values make fog denser; positive values make it more transparent."));
			}
			DrawMultiplierSlider(T(TKEY("volumetric_lighting_intensity"), "Volumetric Lighting Intensity"), settings.vlIntensity);
			DrawGammaOffsetSlider(T(TKEY("volumetric_lighting_gamma_offset"), "Volumetric Lighting Gamma Offset"), settings.vlGammaOffset);
			DrawMultiplierSlider(T(TKEY("sun_glare_intensity"), "Sun Glare Intensity"), settings.sunGlareIntensity);
			ImGui::EndTabItem();
		}

		DrawWaterSettings();

		if (ImGui::BeginTabItem(T(TKEY("tab_multipliers"), "Multipliers"))) {
			activeSettingsPage = SettingsPage::Multipliers;
			if (ImGui::TreeNodeEx(T(TKEY("lighting"), "Lighting"), ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat(T(TKEY("scene_brightness"), "Scene Brightness"), &settings.sceneBrightness, kSceneBrightnessMin, kSceneBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextWrapped("%s", T(TKEY("scene_brightness_tooltip"), "Adjusts lighting and atmosphere together. One preserves the individual controls; lower values darken the scene and higher values brighten it."));
				}
				const bool linearLightingEnabled = globals::features::linearLighting.settings.enableLinearLighting;
				DrawMultiplierSlider(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), settings.ambientLightMult);
				DrawMultiplierSlider(T(TKEY("directional_light_multiplier"), "Directional Light Multiplier"), settings.directionalLightMult);
				DrawMultiplierSlider(T(TKEY("global_point_lighting"), "Global Point Lighting"), settings.pointLightMult);
				DrawLinearMultiplierSlider(T(TKEY("global_point_lighting_linear"), "Global Point Lighting (Linear)"), settings.linearPointLightMult, linearLightingEnabled);
				DrawMultiplierSlider(T(TKEY("spotlights"), "Spotlights"), settings.spotlightMult);
				DrawLinearMultiplierSlider(T(TKEY("spotlights_linear"), "Spotlights (Linear)"), settings.linearSpotlightMult, linearLightingEnabled);
				DrawMultiplierSlider(T(TKEY("omnidirectional_bulbs"), "Omnidirectional Bulbs"), settings.omnidirectionalBulbMult);
				DrawLinearMultiplierSlider(T(TKEY("omnidirectional_bulbs_linear"), "Omnidirectional Bulbs (Linear)"), settings.linearOmnidirectionalBulbMult, linearLightingEnabled);
				ImGui::TreePop();
			}
			if (ImGui::TreeNodeEx(T(TKEY("materials"), "Materials"), ImGuiTreeNodeFlags_DefaultOpen)) {
				DrawMultiplierSlider(T(TKEY("emissive_multiplier"), "Emissive"), settings.emitColorMult);
				DrawMultiplierSlider(T(TKEY("glowmap_multiplier"), "Glowmaps"), settings.glowmapMult);
				DrawMultiplierSlider(T(TKEY("effect_lighting_multiplier"), "Effects"), settings.effectLightingMult);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextWrapped("%s", T(TKEY("effect_lighting_multiplier_tooltip"), "Scales lighting on effect meshes according to their lighting influence."));
				}
				ImGui::TreePop();
			}
			ImGui::EndTabItem();
		}

		DrawDepthOfFieldSettings();
		DrawVanillaBloomSettings();

		auto& volumetricLighting = globals::features::volumetricLighting;
		if (volumetricLighting.loaded && ImGui::BeginTabItem(volumetricLighting.GetDisplayName().c_str())) {
			activeSettingsPage = SettingsPage::VolumetricLighting;
			Util::DrawEmbeddedFeatureSettings(volumetricLighting);
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void CSUtility::SanitizeWaterSettings(WaterSettings& a_settings)
{
	const WaterSettings defaults{};
	a_settings.brightness = Util::ClampFiniteOrDefault(a_settings.brightness, kWaterBrightnessMin, kWaterBrightnessMax, defaults.brightness);
	a_settings.reflectionAmount = Util::ClampFiniteOrDefault(a_settings.reflectionAmount, kWaterAmountMin, kWaterAmountMax, defaults.reflectionAmount);
	a_settings.refractionAmount = Util::ClampFiniteOrDefault(a_settings.refractionAmount, kWaterAmountMin, kWaterAmountMax, defaults.refractionAmount);
	a_settings.sunSpecularMultiplier = Util::ClampFiniteOrDefault(a_settings.sunSpecularMultiplier, kWaterAmountMin, kWaterSunSpecularMax, defaults.sunSpecularMultiplier);
	a_settings.waveAmplitude = Util::ClampFiniteOrDefault(a_settings.waveAmplitude, kWaterAmountMin, kWaterAmountMax, defaults.waveAmplitude);
	a_settings.fresnelMin = Util::ClampFiniteOrDefault(a_settings.fresnelMin, kWaterFresnelMin, kWaterFresnelMax, defaults.fresnelMin);
	a_settings.fresnelMax = Util::ClampFiniteOrDefault(a_settings.fresnelMax, kWaterFresnelMin, kWaterFresnelMax, defaults.fresnelMax);
	a_settings.fresnelMin = std::min(a_settings.fresnelMin, a_settings.fresnelMax);
	a_settings.muddiness = Util::ClampFiniteOrDefault(a_settings.muddiness, kWaterAmountMin, kWaterAmountMax, defaults.muddiness);
	a_settings.causticsStrength = Util::ClampFiniteOrDefault(a_settings.causticsStrength, kWaterAmountMin, kWaterAmountMax, defaults.causticsStrength);
	a_settings.causticsTiling = Util::ClampFiniteOrDefault(a_settings.causticsTiling, kWaterCausticsTilingMin, kWaterCausticsTilingMax, defaults.causticsTiling);
	a_settings.causticsSpeed = Util::ClampFiniteOrDefault(a_settings.causticsSpeed, kWaterAmountMin, kWaterCausticsSpeedMax, defaults.causticsSpeed);
	a_settings.causticsDispersion = Util::ClampFiniteOrDefault(a_settings.causticsDispersion, kWaterAmountMin, kWaterAmountMax, defaults.causticsDispersion);
	a_settings.parallaxStrength = Util::ClampFiniteOrDefault(a_settings.parallaxStrength, kWaterAmountMin, kWaterAmountMax, defaults.parallaxStrength);
	a_settings.parallaxQuality = std::clamp(a_settings.parallaxQuality, kWaterParallaxQualityMin, kWaterParallaxQualityMax);
}

void CSUtility::DrawWaterSettings()
{
	if (!ImGui::BeginTabItem(T(TKEY("tab_water"), "Water")))
		return;

	activeSettingsPage = SettingsPage::Water;
	auto& water = settings.water;
	DrawWaterSlider(T(TKEY("water_brightness"), "Brightness"), water.brightness, kWaterBrightnessMin, kWaterBrightnessMax,
		T(TKEY("water_brightness_tooltip"), "Scales the final water surface brightness."));
	DrawGammaOffsetSlider(T(TKEY("water_gamma_offset"), "Water Gamma Offset"), settings.waterGammaOffset);
	DrawWaterSlider(T(TKEY("water_reflection_amount"), "Reflection Amount"), water.reflectionAmount, kWaterAmountMin, kWaterAmountMax,
		T(TKEY("water_reflection_amount_tooltip"), "Scales environment, cubemap, and screen-space reflections on water."));
	DrawWaterSlider(T(TKEY("water_refraction_amount"), "Refraction Amount"), water.refractionAmount, kWaterAmountMin, kWaterAmountMax,
		T(TKEY("water_refraction_amount_tooltip"), "Scales the distortion applied to the scene viewed through water."));
	DrawWaterSlider(T(TKEY("water_sun_specular_multiplier"), "Sun Specular Multiplier"), water.sunSpecularMultiplier, kWaterAmountMin, kWaterSunSpecularMax,
		T(TKEY("water_sun_specular_multiplier_tooltip"), "Scales the direct sun highlight reflected by the water surface."));
	DrawWaterSlider(T(TKEY("water_wave_amplitude"), "Wave Amplitude"), water.waveAmplitude, kWaterAmountMin, kWaterAmountMax,
		T(TKEY("water_wave_amplitude_tooltip"), "Scales water surface normals, including flowmap and rain ripple detail."));

	DrawWaterSlider(T(TKEY("water_fresnel_min"), "Fresnel Min"), water.fresnelMin, kWaterFresnelMin, water.fresnelMax,
		T(TKEY("water_fresnel_min_tooltip"), "Minimum reflection response when viewing the water surface head-on."));
	DrawWaterSlider(T(TKEY("water_fresnel_max"), "Fresnel Max"), water.fresnelMax, water.fresnelMin, kWaterFresnelMax,
		T(TKEY("water_fresnel_max_tooltip"), "Maximum reflection response at grazing view angles."));
	DrawWaterSlider(T(TKEY("water_muddiness"), "Muddiness"), water.muddiness, kWaterAmountMin, kWaterAmountMax,
		T(TKEY("water_muddiness_tooltip"), "Scales the water tint mixed over the refracted scene. Lower values make water clearer."));

	ImGui::SeparatorText(T("feature.water_effects.name", "Water Effects"));
	DrawWaterSlider(T(TKEY("water_caustics_strength"), "Caustics Strength"), water.causticsStrength, kWaterAmountMin, kWaterAmountMax,
		T(TKEY("water_caustics_strength_tooltip"), "Scales the underwater light pattern contrast. One preserves the current appearance; zero disables caustics."));
	DrawWaterSlider(T(TKEY("water_caustics_tiling"), "Caustics Tiling"), water.causticsTiling, kWaterCausticsTilingMin, kWaterCausticsTilingMax,
		T(TKEY("water_caustics_tiling_tooltip"), "Scales how often the caustics pattern repeats. Higher values create smaller patterns."));
	DrawWaterSlider(T(TKEY("water_caustics_speed"), "Caustics Speed"), water.causticsSpeed, kWaterAmountMin, kWaterCausticsSpeedMax,
		T(TKEY("water_caustics_speed_tooltip"), "Scales caustics animation speed. Zero freezes the pattern."));
	DrawWaterSlider(T(TKEY("water_caustics_dispersion"), "Caustics Color Dispersion"), water.causticsDispersion, kWaterAmountMin, kWaterAmountMax,
		T(TKEY("water_caustics_dispersion_tooltip"), "Scales the separation of colors in caustics. Zero removes color separation."));
	DrawWaterSlider(T(TKEY("water_parallax_strength"), "Parallax Strength"), water.parallaxStrength, kWaterAmountMin, kWaterAmountMax,
		T(TKEY("water_parallax_strength_tooltip"), "Scales the apparent depth of water waves, including flowmaps. One preserves the current appearance; zero disables water parallax."));
	ImGui::SliderInt(T(TKEY("water_parallax_quality"), "Parallax Quality"), &water.parallaxQuality, kWaterParallaxQualityMin, kWaterParallaxQualityMax, "%d", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextWrapped("%s", T(TKEY("water_parallax_quality_tooltip"), "Sets water parallax sampling quality, including flowmaps. 16 matches current quality. Higher values reduce stepping and increase GPU cost."));
	}

	SanitizeWaterSettings(water);
	ImGui::EndTabItem();
}

void CSUtility::DrawVanillaBloomSettings()
{
	if (ImGui::BeginTabItem(T(TKEY("tab_vanilla_bloom"), "Vanilla Bloom"))) {
		activeSettingsPage = SettingsPage::VanillaBloom;
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

void CSUtility::RestoreCurrentPageDefaultSettings()
{
	const Settings defaults{};
	switch (activeSettingsPage) {
	case SettingsPage::Atmosphere:
		settings.skyBrightness = defaults.skyBrightness;
		settings.cloudBrightness = defaults.cloudBrightness;
		settings.skySaturation = defaults.skySaturation;
		settings.cloudSaturation = defaults.cloudSaturation;
		settings.skyGammaOffset = defaults.skyGammaOffset;
		settings.cloudGammaOffset = defaults.cloudGammaOffset;
		settings.effectBrightness = defaults.effectBrightness;
		settings.skyStaticBrightness = defaults.skyStaticBrightness;
		settings.skyStaticTransparency = defaults.skyStaticTransparency;
		settings.fogGammaOffset = defaults.fogGammaOffset;
		settings.fogAlphaGammaOffset = defaults.fogAlphaGammaOffset;
		settings.fogIntensity = defaults.fogIntensity;
		settings.vlGammaOffset = defaults.vlGammaOffset;
		settings.vlIntensity = defaults.vlIntensity;
		settings.sunGlareIntensity = defaults.sunGlareIntensity;
		break;
	case SettingsPage::Water:
		settings.water = defaults.water;
		settings.waterGammaOffset = defaults.waterGammaOffset;
		break;
	case SettingsPage::Multipliers:
		settings.ambientLightMult = defaults.ambientLightMult;
		settings.directionalLightMult = defaults.directionalLightMult;
		settings.pointLightMult = defaults.pointLightMult;
		settings.linearPointLightMult = defaults.linearPointLightMult;
		settings.spotlightMult = defaults.spotlightMult;
		settings.linearSpotlightMult = defaults.linearSpotlightMult;
		settings.omnidirectionalBulbMult = defaults.omnidirectionalBulbMult;
		settings.linearOmnidirectionalBulbMult = defaults.linearOmnidirectionalBulbMult;
		settings.sceneBrightness = defaults.sceneBrightness;
		settings.emitColorMult = defaults.emitColorMult;
		settings.glowmapMult = defaults.glowmapMult;
		settings.effectLightingMult = defaults.effectLightingMult;
		break;
	case SettingsPage::VanillaDepthOfField:
		settings.sceneDof = defaults.sceneDof;
		settings.underwaterDof = defaults.underwaterDof;
		break;
	case SettingsPage::VanillaBloom:
		settings.bloomEnhancement = defaults.bloomEnhancement;
		break;
	case SettingsPage::VolumetricLighting:
		globals::features::volumetricLighting.RestoreDefaultSettings();
		break;
	}
}

bool CSUtility::ReapplyCurrentPageOverrideSettings()
{
	static constexpr std::array<std::string_view, 15> atmosphereKeys{ "skyBrightness", "cloudBrightness", "skySaturation", "cloudSaturation", "skyGammaOffset", "cloudGammaOffset", "effectBrightness", "skyStaticBrightness", "skyStaticTransparency", "fogGammaOffset", "fogAlphaGammaOffset", "fogIntensity", "vlGammaOffset", "vlIntensity", "sunGlareIntensity" };
	static constexpr std::array<std::string_view, 2> waterKeys{ "water", "waterGammaOffset" };
	static constexpr std::array<std::string_view, 12> multiplierKeys{
		"ambientLightMult",
		"directionalLightMult",
		"pointLightMult",
		"linearPointLightMult",
		"spotlightMult",
		"linearSpotlightMult",
		"omnidirectionalBulbMult",
		"linearOmnidirectionalBulbMult",
		"sceneBrightness",
		"emitColorMult",
		"glowmapMult",
		"effectLightingMult"
	};
	static constexpr std::array<std::string_view, 2> depthOfFieldKeys{ "sceneDof", "underwaterDof" };
	static constexpr std::array<std::string_view, 1> bloomKeys{ "bloomEnhancement" };

	switch (activeSettingsPage) {
	case SettingsPage::Atmosphere:
		return ReapplyOverrideSettingsForKeys(atmosphereKeys);
	case SettingsPage::Water:
		return ReapplyOverrideSettingsForKeys(waterKeys);
	case SettingsPage::Multipliers:
		return ReapplyOverrideSettingsForKeys(multiplierKeys);
	case SettingsPage::VanillaDepthOfField:
		return ReapplyOverrideSettingsForKeys(depthOfFieldKeys);
	case SettingsPage::VanillaBloom:
		return ReapplyOverrideSettingsForKeys(bloomKeys);
	case SettingsPage::VolumetricLighting:
		return globals::features::volumetricLighting.ReapplyOverrideSettings();
	}
	return false;
}

void CSUtility::SetupResources()
{
	vanillaPointLightCB = new ConstantBuffer(ConstantBufferDesc<VanillaPointLightData>(), "OSUtility::VanillaPointLightData");
}

CSUtility::PerFrameData CSUtility::GetCommonBufferData() const
{
	Settings sanitizedSettings = settings;
	SanitizeSettings(sanitizedSettings);

	const float brightnessDelta = sanitizedSettings.sceneBrightness - 1.0f;
	const float gammaOffset = -brightnessDelta * kSceneGammaWeight;
	const auto scaleMultiplier = [&](float a_value, float a_weight) {
		return std::clamp(a_value * (1.0f + brightnessDelta * a_weight), kMultiplierMin, kMultiplierMax);
	};

	PerFrameData data{};
	data.skyBrightness = sanitizedSettings.skyBrightness;
	data.cloudBrightness = sanitizedSettings.cloudBrightness;
	data.ambientLightMult = scaleMultiplier(sanitizedSettings.ambientLightMult, kSceneAmbientWeight);
	data.directionalLightMult = scaleMultiplier(sanitizedSettings.directionalLightMult, kSceneDirectionalWeight);
	data.pointLightMult = scaleMultiplier(sanitizedSettings.pointLightMult, kScenePointWeight);
	data.linearPointLightMult = scaleMultiplier(sanitizedSettings.linearPointLightMult, kScenePointWeight);
	data.spotlightMult = sanitizedSettings.spotlightMult;
	data.linearSpotlightMult = sanitizedSettings.linearSpotlightMult;
	data.omnidirectionalBulbMult = sanitizedSettings.omnidirectionalBulbMult;
	data.linearOmnidirectionalBulbMult = sanitizedSettings.linearOmnidirectionalBulbMult;
	data.waterBrightness = sanitizedSettings.water.brightness;
	data.waterReflectionAmount = sanitizedSettings.water.reflectionAmount;
	data.waterRefractionAmount = sanitizedSettings.water.refractionAmount;
	data.waterSunSpecularMultiplier = sanitizedSettings.water.sunSpecularMultiplier;
	data.waterWaveAmplitude = sanitizedSettings.water.waveAmplitude;
	data.waterFresnelMin = sanitizedSettings.water.fresnelMin;
	data.waterFresnelMax = sanitizedSettings.water.fresnelMax;
	data.waterMuddiness = sanitizedSettings.water.muddiness;
	data.emitColorMult = scaleMultiplier(sanitizedSettings.emitColorMult, kSceneEmissiveWeight);
	data.glowmapMult = scaleMultiplier(sanitizedSettings.glowmapMult, kSceneEmissiveWeight);
	data.effectLightingMult = scaleMultiplier(sanitizedSettings.effectLightingMult, kSceneEffectWeight);
	data.skyGammaOffset = sanitizedSettings.skyGammaOffset + gammaOffset * kSceneSkyGammaWeight;
	data.cloudGammaOffset = sanitizedSettings.cloudGammaOffset + gammaOffset * kSceneSkyGammaWeight;
	data.skyStaticTransparency = sanitizedSettings.skyStaticTransparency;
	data.fogGammaOffset = sanitizedSettings.fogGammaOffset + gammaOffset * kSceneFogGammaWeight;
	data.fogAlphaGammaOffset = sanitizedSettings.fogAlphaGammaOffset + gammaOffset * kSceneFogAlphaGammaWeight;
	data.fogIntensity = sanitizedSettings.fogIntensity;
	data.waterGammaOffset = sanitizedSettings.waterGammaOffset + gammaOffset * kSceneWaterGammaWeight;
	data.vlGammaOffset = sanitizedSettings.vlGammaOffset + gammaOffset * kSceneVolumetricGammaWeight;
	data.vlIntensity = sanitizedSettings.vlIntensity;
	data.sunGlareIntensity = sanitizedSettings.sunGlareIntensity;
	data.waterCausticsStrength = sanitizedSettings.water.causticsStrength;
	data.waterCausticsTiling = sanitizedSettings.water.causticsTiling;
	data.waterCausticsSpeed = sanitizedSettings.water.causticsSpeed;
	data.waterCausticsDispersion = sanitizedSettings.water.causticsDispersion;
	data.waterParallaxStrength = sanitizedSettings.water.parallaxStrength;
	data.skySaturation = sanitizedSettings.skySaturation;
	data.cloudSaturation = sanitizedSettings.cloudSaturation;
	data.waterParallaxQuality = static_cast<uint32_t>(sanitizedSettings.water.parallaxQuality);
	return data;
}

void CSUtility::OnWeatherColorsUpdated(RE::Sky* a_sky)
{
	if (!a_sky || !a_sky->currentWeather)
		return;

	const Settings defaults{};
	a_sky->skyColor[RE::TESWeather::ColorTypes::kEffectLighting] *= Util::ClampFiniteOrDefault(settings.effectBrightness, kWeatherBrightnessMin, kWeatherBrightnessMax, defaults.effectBrightness);
	a_sky->skyColor[RE::TESWeather::ColorTypes::kSkyStatics] *= Util::ClampFiniteOrDefault(settings.skyStaticBrightness, kWeatherBrightnessMin, kWeatherBrightnessMax, defaults.skyStaticBrightness);
}

void CSUtility::UpdateVanillaPointLightData(RE::BSRenderPass* a_pass, uint32_t a_lightCount)
{
	if (!vanillaPointLightCB || !a_pass || !a_pass->sceneLights)
		return;

	VanillaPointLightData data{};
	const uint32_t lightCount = std::min(a_lightCount, kMaxVanillaPointLights);
#if defined(_MSC_VER)
	__try
#endif
	{
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
	}
#if defined(_MSC_VER)
	__except (1) {
		// A stale sceneLights entry means the rest of the array is suspect; stop
		// rather than resume. Zeroed flags fall back to vanilla falloff.
	}
#endif

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
