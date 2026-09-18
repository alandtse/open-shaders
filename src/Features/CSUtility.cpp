#include "CSUtility.h"

#include "Bloom.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "LightLimitFix.h"
#include "LinearLighting.h"
#include "ShaderCache.h"
#include "State.h"
#include "UnderwaterDepthOfField.h"
#include "Utils/PointLightFlags.h"
#include "Utils/StringUtils.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#define I18N_KEY_PREFIX "feature.cs_utility."

namespace
{
	constexpr float kSkyBrightnessMin = 0.0f;
	constexpr float kSkyBrightnessMax = 2.0f;
	constexpr float kMultiplierMin = 0.0f;
	constexpr float kMultiplierMax = 5.0f;
	constexpr float kWaterBrightnessMin = 0.0f;
	constexpr float kWaterBrightnessMax = 2.0f;
	constexpr float kWaterAmountMin = 0.0f;
	constexpr float kWaterAmountMax = 2.0f;
	constexpr float kWaterSunSpecularMax = 5.0f;
	constexpr float kWaterFresnelMin = 0.0f;
	constexpr float kWaterFresnelMax = 1.0f;
	constexpr float kFireDisplacementMax = 0.3f;
	constexpr float kFireNoiseScaleMin = 0.01f;
	constexpr float kFireNoiseScaleMax = 1.0f;
	constexpr float kFireSpeedMax = 10.0f;
	constexpr float kFireEmissionMax = 25.0f;
	constexpr float kFireDepthFadeMax = 64.0f;
	constexpr float kFireFresnelPowerMax = 8.0f;
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
		CSUtility::SanitizeWaterSettings(a_settings.water);
		a_settings.fireEffects.displacement = ClampFiniteOrDefault(a_settings.fireEffects.displacement, 0.0f, kFireDisplacementMax, defaults.fireEffects.displacement);
		a_settings.fireEffects.noiseScale = ClampFiniteOrDefault(a_settings.fireEffects.noiseScale, kFireNoiseScaleMin, kFireNoiseScaleMax, defaults.fireEffects.noiseScale);
		a_settings.fireEffects.speed = ClampFiniteOrDefault(a_settings.fireEffects.speed, 0.0f, kFireSpeedMax, defaults.fireEffects.speed);
		a_settings.fireEffects.opacityMin = ClampFiniteOrDefault(a_settings.fireEffects.opacityMin, 0.0f, 0.99f, defaults.fireEffects.opacityMin);
		a_settings.fireEffects.opacityMax = ClampFiniteOrDefault(a_settings.fireEffects.opacityMax, a_settings.fireEffects.opacityMin + 0.01f, 1.0f, defaults.fireEffects.opacityMax);
		a_settings.fireEffects.colorMin = ClampFiniteOrDefault(a_settings.fireEffects.colorMin, 0.0f, 0.99f, defaults.fireEffects.colorMin);
		a_settings.fireEffects.colorMax = ClampFiniteOrDefault(a_settings.fireEffects.colorMax, a_settings.fireEffects.colorMin + 0.01f, 1.0f, defaults.fireEffects.colorMax);
		a_settings.fireEffects.depthFadeDistance = ClampFiniteOrDefault(a_settings.fireEffects.depthFadeDistance, 0.1f, kFireDepthFadeMax, defaults.fireEffects.depthFadeDistance);
		a_settings.fireEffects.fresnelPower = ClampFiniteOrDefault(a_settings.fireEffects.fresnelPower, 0.1f, kFireFresnelPowerMax, defaults.fireEffects.fresnelPower);
		a_settings.fireEffects.edgeFade = ClampFiniteOrDefault(a_settings.fireEffects.edgeFade, 0.01f, 0.49f, defaults.fireEffects.edgeFade);
		a_settings.fireEffects.emission = ClampFiniteOrDefault(a_settings.fireEffects.emission, 0.0f, kFireEmissionMax, defaults.fireEffects.emission);
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

	void DrawWaterSlider(const char* a_label, float& a_value, float a_min, float a_max, const char* a_tooltip)
	{
		ImGui::SliderFloat(a_label, &a_value, a_min, a_max, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextWrapped("%s", a_tooltip);
		}
	}

	bool ContainsFireKeyword(std::string_view a_value)
	{
		const std::string value = Util::ToLowerAscii(a_value);
		static constexpr std::array keywords{ "fire", "flame", "torch", "ember", "burn" };
		return std::any_of(keywords.begin(), keywords.end(), [&](std::string_view a_keyword) { return value.contains(a_keyword); });
	}

	bool IsKnownFirePermutation(uint32_t a_descriptor)
	{
		using Flags = SIE::ShaderCache::EffectShaderFlags;
		auto has = [&](Flags a_flag) { return (a_descriptor & static_cast<uint32_t>(a_flag)) != 0; };
		return has(Flags::AddBlend) &&
		       ((has(Flags::Soft) && has(Flags::GrayscaleToColor) && has(Flags::GrayscaleToAlpha)) ||
				   (has(Flags::Particles) && has(Flags::TexCoordIndex) && has(Flags::IndexedTexture)));
	}

	bool IsParticlePermutation(uint32_t a_descriptor)
	{
		using Flags = SIE::ShaderCache::EffectShaderFlags;
		return (a_descriptor & static_cast<uint32_t>(Flags::Particles)) != 0;
	}

	bool IsFireParticleGeometry(std::string_view a_name)
	{
		const std::string name = Util::ToLowerAscii(a_name);
		if (name.contains("ember") || name.contains("smoke") || name.contains("spark"))
			return false;

		return name.contains("fireballcore") || name.contains("torchfire") || name.contains("flame");
	}

	bool IsAuxiliaryTorchEffect(std::string_view a_name)
	{
		const std::string name = Util::ToLowerAscii(a_name);
		return name.contains("glowmesh") || name.contains("glowaddmesh") ||
		       name.contains("heatrefraction") || name.contains("ember") ||
		       name.contains("smoke") || name.contains("spark");
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
	muddiness)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CSUtility::FireEffectSettings,
	enabled,
	displacement,
	noiseScale,
	speed,
	opacityMin,
	opacityMax,
	colorMin,
	colorMax,
	depthFadeDistance,
	fresnelPower,
	edgeFade,
	emission,
	lowColor,
	highColor)

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
	water,
	fireEffects,
	sceneDof,
	underwaterDof,
	bloomEnhancement)

void CSUtility::DrawSettings()
{
	if (ImGui::BeginTabBar("##CSUtilityTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_atmosphere"), "Atmosphere"))) {
			activeSettingsPage = SettingsPage::Atmosphere;
			ImGui::SliderFloat(T(TKEY("sky_brightness"), "Sky Brightness"), &settings.skyBrightness, kSkyBrightnessMin, kSkyBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndTabItem();
		}

		DrawWaterSettings();

		if (ImGui::BeginTabItem(T(TKEY("tab_fire_effects"), "Fire Effects"))) {
			activeSettingsPage = SettingsPage::FireEffects;
			auto& fire = settings.fireEffects;
			ImGui::Checkbox(T(TKEY("fire_effects_enable"), "Enable Procedural Fire"), &fire.enabled);
			ImGui::SliderFloat(T(TKEY("fire_displacement"), "Displacement"), &fire.displacement, 0.0f, kFireDisplacementMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_noise_scale"), "Noise Scale"), &fire.noiseScale, kFireNoiseScaleMin, kFireNoiseScaleMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_speed"), "Speed"), &fire.speed, 0.0f, kFireSpeedMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_opacity_min"), "Opacity Threshold"), &fire.opacityMin, 0.0f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_opacity_max"), "Opacity Softness"), &fire.opacityMax, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_color_min"), "Color Threshold"), &fire.colorMin, 0.0f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_color_max"), "Color Softness"), &fire.colorMax, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_depth_fade"), "Intersection Fade"), &fire.depthFadeDistance, 0.1f, kFireDepthFadeMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_fresnel_power"), "Edge Fresnel"), &fire.fresnelPower, 0.1f, kFireFresnelPowerMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_edge_fade"), "Top / Bottom Fade"), &fire.edgeFade, 0.01f, 0.49f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("fire_emission"), "Emission"), &fire.emission, 0.0f, kFireEmissionMax, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::ColorEdit3(T(TKEY("fire_low_color"), "Outer Color"), reinterpret_cast<float*>(&fire.lowColor));
			ImGui::ColorEdit3(T(TKEY("fire_high_color"), "Core Color"), reinterpret_cast<float*>(&fire.highColor));
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_multipliers"), "Multipliers"))) {
			activeSettingsPage = SettingsPage::Multipliers;
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

void CSUtility::SanitizeWaterSettings(WaterSettings& a_settings)
{
	const WaterSettings defaults{};
	a_settings.brightness = ClampFiniteOrDefault(a_settings.brightness, kWaterBrightnessMin, kWaterBrightnessMax, defaults.brightness);
	a_settings.reflectionAmount = ClampFiniteOrDefault(a_settings.reflectionAmount, kWaterAmountMin, kWaterAmountMax, defaults.reflectionAmount);
	a_settings.refractionAmount = ClampFiniteOrDefault(a_settings.refractionAmount, kWaterAmountMin, kWaterAmountMax, defaults.refractionAmount);
	a_settings.sunSpecularMultiplier = ClampFiniteOrDefault(a_settings.sunSpecularMultiplier, kWaterAmountMin, kWaterSunSpecularMax, defaults.sunSpecularMultiplier);
	a_settings.waveAmplitude = ClampFiniteOrDefault(a_settings.waveAmplitude, kWaterAmountMin, kWaterAmountMax, defaults.waveAmplitude);
	a_settings.fresnelMin = ClampFiniteOrDefault(a_settings.fresnelMin, kWaterFresnelMin, kWaterFresnelMax, defaults.fresnelMin);
	a_settings.fresnelMax = ClampFiniteOrDefault(a_settings.fresnelMax, kWaterFresnelMin, kWaterFresnelMax, defaults.fresnelMax);
	a_settings.fresnelMin = std::min(a_settings.fresnelMin, a_settings.fresnelMax);
	a_settings.muddiness = ClampFiniteOrDefault(a_settings.muddiness, kWaterAmountMin, kWaterAmountMax, defaults.muddiness);
}

void CSUtility::DrawWaterSettings()
{
	if (!ImGui::BeginTabItem(T(TKEY("tab_water"), "Water")))
		return;

	activeSettingsPage = SettingsPage::Water;
	auto& water = settings.water;
	DrawWaterSlider(T(TKEY("water_brightness"), "Brightness"), water.brightness, kWaterBrightnessMin, kWaterBrightnessMax,
		T(TKEY("water_brightness_tooltip"), "Scales the final water surface brightness."));
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
		break;
	case SettingsPage::Water:
		settings.water = defaults.water;
		break;
	case SettingsPage::Multipliers:
		settings.directionalLightMult = defaults.directionalLightMult;
		settings.pointLightMult = defaults.pointLightMult;
		settings.linearPointLightMult = defaults.linearPointLightMult;
		settings.spotlightMult = defaults.spotlightMult;
		settings.linearSpotlightMult = defaults.linearSpotlightMult;
		settings.omnidirectionalBulbMult = defaults.omnidirectionalBulbMult;
		settings.linearOmnidirectionalBulbMult = defaults.linearOmnidirectionalBulbMult;
		break;
	case SettingsPage::VanillaDepthOfField:
		settings.sceneDof = defaults.sceneDof;
		settings.underwaterDof = defaults.underwaterDof;
		break;
	case SettingsPage::VanillaBloom:
		settings.bloomEnhancement = defaults.bloomEnhancement;
		break;
	case SettingsPage::FireEffects:
		settings.fireEffects = defaults.fireEffects;
		break;
	}
}

bool CSUtility::ReapplyCurrentPageOverrideSettings()
{
	static constexpr std::array<std::string_view, 1> atmosphereKeys{ "skyBrightness" };
	static constexpr std::array<std::string_view, 1> waterKeys{ "water" };
	static constexpr std::array<std::string_view, 7> multiplierKeys{
		"directionalLightMult",
		"pointLightMult",
		"linearPointLightMult",
		"spotlightMult",
		"linearSpotlightMult",
		"omnidirectionalBulbMult",
		"linearOmnidirectionalBulbMult"
	};
	static constexpr std::array<std::string_view, 2> depthOfFieldKeys{ "sceneDof", "underwaterDof" };
	static constexpr std::array<std::string_view, 1> bloomKeys{ "bloomEnhancement" };
	static constexpr std::array<std::string_view, 1> fireEffectKeys{ "fireEffects" };

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
	case SettingsPage::FireEffects:
		return ReapplyOverrideSettingsForKeys(fireEffectKeys);
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

	PerFrameData data{};
	data.skyBrightness = sanitizedSettings.skyBrightness;
	data.directionalLightMult = sanitizedSettings.directionalLightMult;
	data.pointLightMult = sanitizedSettings.pointLightMult;
	data.linearPointLightMult = sanitizedSettings.linearPointLightMult;
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
	data.enableFireEffects = sanitizedSettings.fireEffects.enabled;
	data.fireDisplacement = sanitizedSettings.fireEffects.displacement;
	data.fireNoiseScale = sanitizedSettings.fireEffects.noiseScale;
	data.fireSpeed = sanitizedSettings.fireEffects.speed;
	data.fireOpacityMin = sanitizedSettings.fireEffects.opacityMin;
	data.fireOpacityMax = sanitizedSettings.fireEffects.opacityMax;
	data.fireColorMin = sanitizedSettings.fireEffects.colorMin;
	data.fireColorMax = sanitizedSettings.fireEffects.colorMax;
	data.fireDepthFadeDistance = sanitizedSettings.fireEffects.depthFadeDistance;
	data.fireFresnelPower = sanitizedSettings.fireEffects.fresnelPower;
	data.fireEdgeFade = sanitizedSettings.fireEffects.edgeFade;
	data.fireEmission = sanitizedSettings.fireEffects.emission;
	data.fireLowColor = sanitizedSettings.fireEffects.lowColor;
	data.fireHighColor = sanitizedSettings.fireEffects.highColor;
	return data;
}

void CSUtility::ModifyEffect(RE::BSRenderPass* a_pass) const
{
	auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
	const auto fireDescriptor = static_cast<uint32_t>(State::ExtraShaderDescriptors::FireEffect);
	descriptor &= ~fireDescriptor;

	if (!settings.fireEffects.enabled || !a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return;

	const auto pixelDescriptor = globals::state->currentPixelDescriptor;
	const bool particlePermutation = IsParticlePermutation(pixelDescriptor);
	const bool knownFirePermutation = IsKnownFirePermutation(pixelDescriptor);
	auto* effectProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSEffectShaderPropertyRTTI.get() ?
	                           static_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty) :
	                           nullptr;
	const auto* material = effectProperty ? effectProperty->GetMaterial() : nullptr;
	const auto* parent = a_pass->geometry->parent;
	if (IsAuxiliaryTorchEffect(a_pass->geometry->name.c_str()))
		return;

	const bool fireParticle = particlePermutation && IsFireParticleGeometry(a_pass->geometry->name.c_str());
	const bool fireAsset = !particlePermutation && (ContainsFireKeyword(a_pass->geometry->name.c_str()) ||
													   (parent && ContainsFireKeyword(parent->name.c_str())) ||
													   (material && (ContainsFireKeyword(material->sourceTexturePath.c_str()) ||
																		ContainsFireKeyword(material->greyscaleTexturePath.c_str()))));

	if (fireParticle || (!particlePermutation && (knownFirePermutation || fireAsset)))
		descriptor |= fireDescriptor;
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
