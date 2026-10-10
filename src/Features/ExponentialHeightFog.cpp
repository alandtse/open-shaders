#include "ExponentialHeightFog.h"

#include "Deferred.h"
#include "Features/CSUtility.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

#include <numbers>

#define I18N_KEY_PREFIX "feature.exp_height_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ExponentialHeightFog::Settings,
	enabled,
	useDynamicCubemaps,
	startDistance,
	fogHeight,
	fogHeightFalloff,
	fogDensity,
	fogHeight2,
	fogHeightFalloff2,
	fogDensity2,
	directionalInscatteringMultiplier,
	directionalInscatteringAnisotropy,
	useSkyIBL,
	inscatteringTint,
	cubemapMipLevel,
	sunlightAttenuationAmount,
	respectVanillaFogFade,
	disableVanillaFog,
	fogInscatteringColor,
	originalFogColorAmount,
	volumetricFogEnabled,
	volumetricGridPixelSize,
	volumetricGridSizeZ,
	volumetricFogDistance,
	volumetricFogStartDistance,
	volumetricFogNearFadeInDistance,
	volumetricFogExtinctionScale,
	volumetricFogScatteringDistribution,
	volumetricFogAlbedo,
	volumetricFogEmissive,
	volumetricDirectionalScatteringIntensity,
	volumetricShadowBias,
	volumetricDepthDistributionScale,
	volumetricSkyLightingIntensity,
	volumetricHistoryWeight,
	volumetricHistoryMissSampleCount,
	volumetricSampleJitterMultiplier,
	volumetricUpsampleJitterMultiplier,
	volumetricLocalLightScatteringIntensity,
	useVanillaFogSettings,
	vanillaFogStrength,
	fogLightingInfluence,
	distanceHazeMaxOpacity,
	distanceHazeStartDistance,
	distanceHazeFadeDistance,
	volumetricNearGridDistance,
	volumetricFarGridPixelSize,
	volumetricFarGridSizeZ,
	volumetricFogNoiseScale,
	volumetricFogNoiseThreshold,
	volumetricFogNoiseVelocity)

namespace
{
	constexpr float kMinimumFogRange = 1.0f;
	constexpr float kMinimumFogPower = 0.01f;
	constexpr float kMinimumFogTransmittance = 0.0001f;
	constexpr float kReferenceOpacityFraction = 0.5f;
	constexpr float kMaximumWeatherHistoryChange = 0.1f;
	constexpr float kAnalyticalExtinctionScale = 0.001f * std::numbers::ln2_v<float> * std::numbers::ln2_v<float>;
	constexpr float4 kFallbackFogColor{ 0.85f, 0.88f, 0.92f, 1.0f };

	constexpr float MinDistanceHazeFadeDistance = 1.0f;
	constexpr float MaxDistanceHazeDistance = 200000.0f;

	void ClampDistanceHazeSettings(ExponentialHeightFog::Settings& settings)
	{
		const ExponentialHeightFog::Settings defaults{};
		settings.distanceHazeMaxOpacity = Util::ClampFinite(settings.distanceHazeMaxOpacity, 0.0f, 1.0f, defaults.distanceHazeMaxOpacity);
		settings.distanceHazeStartDistance = Util::ClampFinite(settings.distanceHazeStartDistance, 0.0f, MaxDistanceHazeDistance, defaults.distanceHazeStartDistance);
		settings.distanceHazeFadeDistance = Util::ClampFinite(settings.distanceHazeFadeDistance, MinDistanceHazeFadeDistance, MaxDistanceHazeDistance, defaults.distanceHazeFadeDistance);
	}

	bool CanReuseFogHistory(const ExponentialHeightFog::Settings& current, const ExponentialHeightFog::Settings& previous)
	{
		using Settings = ExponentialHeightFog::Settings;
		if (current.useVanillaFogSettings != previous.useVanillaFogSettings)
			return false;
		for (const auto field : {
				 &Settings::vanillaFogStrength, &Settings::fogLightingInfluence,
				 &Settings::fogDensity, &Settings::fogHeight, &Settings::fogHeightFalloff,
				 &Settings::fogDensity2, &Settings::fogHeight2, &Settings::fogHeightFalloff2,
				 &Settings::volumetricNearGridDistance,
				 &Settings::volumetricFogDistance, &Settings::volumetricFogStartDistance,
				 &Settings::volumetricFogNearFadeInDistance, &Settings::volumetricFogExtinctionScale,
				 &Settings::volumetricDepthDistributionScale }) {
			if (current.*field != previous.*field)
				return false;
		}
		if (current.useVanillaFogSettings) {
			if (std::abs(current.vanillaFogDensity - previous.vanillaFogDensity) >
				kMaximumWeatherHistoryChange * std::max(current.vanillaFogDensity, previous.vanillaFogDensity))
				return false;
			auto weatherHistoryMatches = [](float value, float previousValue) {
				return std::abs(value - previousValue) <= kMaximumWeatherHistoryChange * std::max({ 1.0f, std::abs(value), std::abs(previousValue) });
			};
			for (const auto field : { &Settings::vanillaFogNear, &Settings::vanillaFogFar, &Settings::vanillaFogMaxOpacity, &Settings::vanillaFogPower }) {
				if (!weatherHistoryMatches(current.*field, previous.*field))
					return false;
			}
			for (const auto field : { &Settings::vanillaFogNearColor, &Settings::vanillaFogFarColor }) {
				const auto& color = current.*field;
				const auto& previousColor = previous.*field;
				if (!weatherHistoryMatches(color.x, previousColor.x) ||
					!weatherHistoryMatches(color.y, previousColor.y) ||
					!weatherHistoryMatches(color.z, previousColor.z))
					return false;
			}
		}
		return true;
	}

	float Halton(uint32_t a_index, uint32_t a_base)
	{
		float result = 0.0f;
		float invBase = 1.0f / static_cast<float>(a_base);
		float fraction = invBase;
		while (a_index > 0) {
			result += static_cast<float>(a_index % a_base) * fraction;
			a_index /= a_base;
			fraction *= invBase;
		}
		return result;
	}

	std::vector<std::pair<const char*, const char*>> LightScatteringDefines(bool a_far)
	{
		std::vector<std::pair<const char*, const char*>> defines;
		if (a_far)
			defines.emplace_back("VOLUMETRIC_FOG_FAR_GRID", "");
		if (globals::features::lightLimitFix.loaded)
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		if (globals::features::terrainShadows.loaded)
			defines.emplace_back("TERRAIN_SHADOWS", "");
		if (globals::features::cloudShadows.loaded)
			defines.emplace_back("CLOUD_SHADOWS", "");
		return defines;
	}
}

void ExponentialHeightFog::RestoreDefaultSettings()
{
	settings = {};
}

void ExponentialHeightFog::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampDistanceHazeSettings(settings);
	settings.vanillaFogStrength = std::clamp(std::isfinite(settings.vanillaFogStrength) ? settings.vanillaFogStrength : Settings{}.vanillaFogStrength, 0.0f, 4.0f);
	settings.fogLightingInfluence = std::clamp(std::isfinite(settings.fogLightingInfluence) ? settings.fogLightingInfluence : Settings{}.fogLightingInfluence, 0.0f, 1.0f);
}

void ExponentialHeightFog::SaveSettings(json& o_json)
{
	o_json = settings;
}

ExponentialHeightFog::Settings ExponentialHeightFog::GetCommonBufferData() const
{
	Settings data = settings;
	data.enabled = data.enabled && !globals::state->isMapMenuOpen;
	ClampDistanceHazeSettings(data);
	data.vanillaFogDensity = 0.0f;

	const auto* sky = globals::game::sky;
	const bool hasUnboundedFogRange = sky && (sky->fogNear == std::numeric_limits<float>::infinity() || sky->fogFar == std::numeric_limits<float>::infinity());
	const float fogNear = sky ? std::max(std::isfinite(sky->fogNear) ? sky->fogNear : 0.0f, 0.0f) : 0.0f;
	const float fogFar = sky ? std::max(std::isfinite(sky->fogFar) ? sky->fogFar : fogNear + kMinimumFogRange, fogNear + kMinimumFogRange) : Settings{}.vanillaFogFar;
	const float fogPower = sky ? std::max(std::isfinite(sky->fogPower) ? sky->fogPower : 1.0f, kMinimumFogPower) : 1.0f;
	const float fogClamp = sky && !hasUnboundedFogRange ? std::clamp(std::isfinite(sky->fogClamp) ? sky->fogClamp : 0.0f, 0.0f, 1.0f - kMinimumFogTransmittance) : 0.0f;

	data.vanillaFogNear = fogNear;
	data.vanillaFogFar = fogFar;
	data.vanillaFogMaxOpacity = fogClamp;
	data.vanillaFogPower = fogPower;
	data.vanillaFogNearColor = kFallbackFogColor;
	data.vanillaFogFarColor = kFallbackFogColor;
	if (sky) {
		auto sanitizeColor = [](const RE::NiColor& color, const float4& fallback) {
			return float4{
				std::max(std::isfinite(color.red) ? color.red : fallback.x, 0.0f),
				std::max(std::isfinite(color.green) ? color.green : fallback.y, 0.0f),
				std::max(std::isfinite(color.blue) ? color.blue : fallback.z, 0.0f), 1.0f
			};
		};
		data.vanillaFogFarColor = sanitizeColor(sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kFogFar)], kFallbackFogColor);
		data.vanillaFogNearColor = sanitizeColor(sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kFogNear)], kFallbackFogColor);
	}
	if (!data.useVanillaFogSettings)
		return data;

	data.disableVanillaFog = 1;
	data.startDistance = fogNear;
	const float targetOpacity = data.vanillaFogMaxOpacity * kReferenceOpacityFraction;
	const float normalizedReference = std::pow(targetOpacity, 1.0f / fogPower);
	const float referenceDistance = std::max((fogFar - fogNear) * normalizedReference, kMinimumFogRange);
	const auto utilityData = globals::features::csUtility.GetCommonBufferData();
	const float adjustedOpacity = std::clamp(targetOpacity * utilityData.fogIntensity, 0.0f, 1.0f);
	const auto linearLightingData = globals::features::linearLighting.GetCommonBufferData();
	const float calibratedOpacity = linearLightingData.enableLinearLighting ?
	                                    LinearLighting::DecodeAuthoredColor(RE::NiColor{ adjustedOpacity, adjustedOpacity, adjustedOpacity }).red :
	                                    adjustedOpacity;
	data.vanillaFogDensity = -std::log(std::max(1.0f - calibratedOpacity, kMinimumFogTransmittance)) / (referenceDistance * kAnalyticalExtinctionScale);
	return data;
}

void ExponentialHeightFog::DrawSettings()
{
	Util::CheckboxFlag(T(TKEY("enable_exp_height_fog"), "Enable Exponential Height Fog"), settings.enabled);
	if (!ImGui::BeginTabBar("##ExponentialHeightFogTabs"))
		return;

	if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
		ImGui::BeginDisabled(settings.enabled == 0);
		DrawGeneralSettings();
		ImGui::EndDisabled();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem(T(TKEY("volumetric_fog"), "Volumetric Fog"))) {
		ImGui::BeginDisabled(settings.enabled == 0);
		DrawVolumetricSettings();
		ImGui::EndDisabled();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

void ExponentialHeightFog::DrawGeneralSettings()
{
	Util::CheckboxFlag(T(TKEY("use_vanilla_fog_settings"), "Follow Vanilla Fog"), settings.useVanillaFogSettings);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_vanilla_fog_settings_tooltip"), "Derives fog density, start distance, and colors from the active weather while keeping exponential height falloff. Weather Fog Strength scales density. Replaces vanilla distance fog."));
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0);
	ImGui::SliderFloat(T(TKEY("vanilla_strength"), "Weather Fog Strength"), &settings.vanillaFogStrength, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("lighting_influence"), "Weather Lighting Influence"), &settings.fogLightingInfluence, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("density_height"), "Density and Height"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::SliderFloat(T(TKEY("fog_density"), "Fog Density"), &settings.fogDensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("start_distance"), "Start Distance"), &settings.startDistance, 0.0f, 100000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::SliderFloat(T(TKEY("fog_height"), "Fog Height"), &settings.fogHeight, -22000.0f, 22000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("fog_height_falloff"), "Fog Height Falloff"), &settings.fogHeightFalloff, 0.001f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	if (ImGui::TreeNode(T(TKEY("second_fog_layer"), "Second Fog Layer"))) {
		ImGui::SliderFloat(T(TKEY("fog_height_2"), "Fog Height 2"), &settings.fogHeight2, -22000.0f, 22000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("fog_height_falloff_2"), "Fog Height Falloff 2"), &settings.fogHeightFalloff2, 0.001f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("fog_density_2"), "Fog Density 2"), &settings.fogDensity2, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("second_fog_layer_tooltip"),
								  "Adds a second stacked exponential height fog layer with its own base height, density and height falloff.\n"
								  "The two line integrals are summed.\n"
								  "Use it for high-altitude haze above the ground layer or a distinct low-lying ground fog."));
		}
		ImGui::TreePop();
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0 && settings.volumetricFogEnabled == 0);
	ImGui::SliderFloat(T(TKEY("volumetric_extinction_scale"), "Extinction Scale"), &settings.volumetricFogExtinctionScale, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_extinction_scale_tooltip"), "Scales fog density when following vanilla fog, including when volumetric fog is off. In manual mode, affects only volumetric fog."));
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	ImGui::ColorEdit4(T(TKEY("fog_inscattering_color"), "Fog Inscattering Color"), (float*)&settings.fogInscatteringColor);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::SliderFloat(T(TKEY("original_fog_color_amount"), "Original Fog Color Amount"), &settings.originalFogColorAmount, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	ImGui::SliderFloat(T(TKEY("dir_inscattering_mul"), "Directional Light Inscattering Multiplier"), &settings.directionalInscatteringMultiplier, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::BeginDisabled(settings.directionalInscatteringMultiplier <= 0.0f);
	ImGui::SliderFloat(T(TKEY("dir_inscattering_anisotropy"), "Directional Light Inscattering Anisotropy"), &settings.directionalInscatteringAnisotropy, -0.99f, 0.99f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dir_inscattering_anisotropy_tooltip"),
							  "Controls the asymmetry of inscattering via the Henyey-Greenstein phase function.\n"
							  "Positive values produce forward scattering (glow around sun).\n"
							  "Zero is isotropic. Negative values produce back scattering."));
	}
	ImGui::EndDisabled();
	ImGui::SliderFloat(T(TKEY("sunlight_attenuation"), "Sunlight Attenuation Amount"), &settings.sunlightAttenuationAmount, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("vanilla_fog"), "Vanilla Fog"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	Util::CheckboxFlag(T(TKEY("disable_vanilla_fog"), "Disable Vanilla Fog"), settings.disableVanillaFog);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("disable_vanilla_fog_tooltip"), "Disables the vanilla fog entirely. Only exponential height fog will be applied."));
	}
	ImGui::EndDisabled();
	Util::CheckboxFlag(T(TKEY("apply_vanilla_fade"), "Apply Vanilla Fade"), settings.respectVanillaFogFade);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("apply_vanilla_fade_tooltip"), "Applies vanilla fade brightness to exponential height fog."));
	}

	ImGui::SeparatorText(T("feature.dynamic_cubemaps.name", "Dynamic Cubemaps"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 || !globals::features::dynamicCubemaps.loaded);
	Util::CheckboxFlag(T(TKEY("use_sky_ibl"), "Use Sky IBL for Exterior Inscattering"), settings.useSkyIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_sky_ibl_tooltip"), "Adds the sky IBL color to the fog inscattering in exteriors. Requires the Image Based Lighting feature."));
	}
	Util::CheckboxFlag(T(TKEY("use_dynamic_cubemaps"), "Use Dynamic Cubemaps for Interior Inscattering"), settings.useDynamicCubemaps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_tooltip"), "Adds the dynamic environment cubemap color to the fog inscattering in interiors."));
	}
	ImGui::BeginDisabled(settings.useDynamicCubemaps == 0);
	ImGui::ColorEdit4(T(TKEY("inscattering_cubemap_tint"), "Inscattering Cubemap Tint"), (float*)&settings.inscatteringTint);
	ImGui::BeginDisabled(settings.inscatteringTint.w <= 0.0f);
	ImGui::SliderFloat(T(TKEY("cubemap_mip_level"), "Cubemap Mip Level"), &settings.cubemapMipLevel, 1.0f, 8.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("distance_haze"), "Distance Haze"));
	ImGui::SliderFloat(T(TKEY("distance_haze_max_opacity"), "Haze Maximum Opacity"), &settings.distanceHazeMaxOpacity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_max_opacity_tooltip"), "Adds haze at all heights using the fog colors above, while preserving dense height fog. Zero disables distance haze."));
	}
	ImGui::SliderFloat(T(TKEY("distance_haze_start_distance"), "Haze Start Distance"), &settings.distanceHazeStartDistance, 0.0f, MaxDistanceHazeDistance, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_start_distance_tooltip"), "Horizontal distance from the camera where haze begins, in game units. Independent of height fog Start Distance."));
	}
	ImGui::SliderFloat(T(TKEY("distance_haze_fade_distance"), "Haze Fade Distance"), &settings.distanceHazeFadeDistance, MinDistanceHazeFadeDistance, MaxDistanceHazeDistance, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_fade_distance_tooltip"), "Horizontal distance beyond Haze Start Distance over which haze smoothly reaches its maximum opacity, in game units."));
	}
}

void ExponentialHeightFog::DrawVolumetricSettings()
{
	Util::CheckboxFlag(T(TKEY("enable_volumetric_fog"), "Enable Volumetric Fog"), settings.volumetricFogEnabled);
	ImGui::BeginDisabled(settings.volumetricFogEnabled == 0);
	ImGui::SliderFloat(T(TKEY("volumetric_view_distance"), "Volumetric View Distance"), &settings.volumetricFogDistance, 1000.0f, 200000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("volumetric_near_grid_distance"), "Near Grid Distance"), &settings.volumetricNearGridDistance, 256.0f, 50000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_near_grid_distance_tooltip"),
							  "Distance covered by the full-resolution near volume.\n"
							  "A second, quarter-lattice far volume covers the remaining distance up to the Volumetric View Distance.\n"
							  "Smaller values improve near-field resolution; larger values move the low-resolution far volume farther away."));
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::SliderFloat(T(TKEY("volumetric_start_distance"), "Volumetric Start Distance"), &settings.volumetricFogStartDistance, 0.0f, 20000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("near_fade_in_distance"), "Near Fade In Distance"), &settings.volumetricFogNearFadeInDistance, 0.0f, 20000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	ImGui::ColorEdit4(T(TKEY("volumetric_albedo"), "Volumetric Albedo"), (float*)&settings.volumetricFogAlbedo);
	ImGui::ColorEdit4(T(TKEY("volumetric_emissive"), "Volumetric Emissive"), (float*)&settings.volumetricFogEmissive);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	ImGui::SliderFloat(T(TKEY("directional_scattering_intensity"), "Directional Scattering Intensity"), &settings.volumetricDirectionalScatteringIntensity, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::BeginDisabled(settings.volumetricDirectionalScatteringIntensity <= 0.0f);
	ImGui::SliderFloat(T(TKEY("directional_shadow_bias"), "Directional Shadow Bias"), &settings.volumetricShadowBias, 0.0f, 0.05f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::SliderFloat(T(TKEY("sky_lighting_scattering_intensity"), "Sky Lighting Scattering Intensity"), &settings.volumetricSkyLightingIntensity, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::BeginDisabled(!globals::features::lightLimitFix.loaded);
	ImGui::SliderFloat(T(TKEY("local_light_scattering_intensity"), "Local Light Scattering Intensity"), &settings.volumetricLocalLightScatteringIntensity, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	const bool hasScattering = settings.volumetricDirectionalScatteringIntensity > 0.0f || settings.volumetricSkyLightingIntensity > 0.0f ||
	                           (globals::features::lightLimitFix.loaded && settings.volumetricLocalLightScatteringIntensity > 0.0f);
	ImGui::BeginDisabled(!hasScattering);
	ImGui::SliderFloat(T(TKEY("volumetric_scattering_distribution"), "Volumetric Scattering Distribution"), &settings.volumetricFogScatteringDistribution, -0.9f, 0.9f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	if (ImGui::TreeNode(T(TKEY("volumetric_noise"), "Volumetric Noise"))) {
		ImGui::SliderFloat(T(TKEY("volumetric_noise_scale"), "Noise Scale"), &settings.volumetricFogNoiseScale, 0.0f, 0.01f, "%.6f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("volumetric_noise_threshold"), "Noise Threshold"), &settings.volumetricFogNoiseThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat3(T(TKEY("volumetric_noise_velocity"), "Noise Velocity"), &settings.volumetricFogNoiseVelocity.x, -1.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("volumetric_noise_tooltip"),
								  "Modulates the volumetric fog density with a 3D value noise field.\n"
								  "Noise Scale: spatial frequency of the fog clumps (0 = disabled).\n"
								  "Noise Threshold: soft cutoff that carves clumps out of the noise.\n"
								  "Noise Velocity: animation drift of the noise field, scaled by time."));
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode(T(TKEY("debug"), "Quality and Temporal Filtering"))) {
		uint32_t minGridPixelSize = 4;
		uint32_t maxGridPixelSize = 64;
		uint32_t minGridSizeZ = 16;
		uint32_t maxGridSizeZ = 160;
		ImGui::SliderScalar(T(TKEY("grid_pixel_size"), "Grid Pixel Size"), ImGuiDataType_U32, &settings.volumetricGridPixelSize, &minGridPixelSize, &maxGridPixelSize, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderScalar(T(TKEY("grid_depth_slices"), "Grid Depth Slices"), ImGuiDataType_U32, &settings.volumetricGridSizeZ, &minGridSizeZ, &maxGridSizeZ, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderScalar(T(TKEY("far_grid_pixel_size"), "Far Grid Pixel Size"), ImGuiDataType_U32, &settings.volumetricFarGridPixelSize, &minGridPixelSize, &maxGridPixelSize, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderScalar(T(TKEY("far_grid_depth_slices"), "Far Grid Depth Slices"), ImGuiDataType_U32, &settings.volumetricFarGridSizeZ, &minGridSizeZ, &maxGridSizeZ, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("depth_distribution_scale"), "Depth Distribution Scale"), &settings.volumetricDepthDistributionScale, 1.0f, 128.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		const bool temporalReprojection = Util::GetTemporal();
		ImGui::BeginDisabled(!temporalReprojection);
		ImGui::SliderFloat(T(TKEY("temporal_history_weight"), "Temporal History Weight"), &settings.volumetricHistoryWeight, 0.0f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		uint32_t minHistoryMissSampleCount = 1;
		uint32_t maxHistoryMissSampleCount = 16;
		ImGui::SliderScalar(T(TKEY("history_miss_samples"), "History Miss Samples"), ImGuiDataType_U32, &settings.volumetricHistoryMissSampleCount, &minHistoryMissSampleCount, &maxHistoryMissSampleCount, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::BeginDisabled(!temporalReprojection);
		ImGui::SliderFloat(T(TKEY("sample_jitter_multiplier"), "Sample Jitter Multiplier"), &settings.volumetricSampleJitterMultiplier, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("sample_jitter_multiplier_tooltip"),
								  "Matches UE's r.VolumetricFog.LightScatteringSampleJitterMultiplier.\n"
								  "Adds per-voxel random offset on top of the Halton sequence.\n"
								  "0 = UE default; nonzero values need stronger temporal filtering."));
		}
		ImGui::EndDisabled();
		ImGui::SliderFloat(T(TKEY("upsample_jitter_multiplier"), "Upsample Jitter Multiplier"), &settings.volumetricUpsampleJitterMultiplier, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("upsample_jitter_multiplier_tooltip"),
								  "Matches UE's r.VolumetricFog.UpsampleJitterMultiplier.\n"
								  "Jitters the final 3D fog lookup in screen space to hide\n"
								  "low-resolution froxel pixelization. 0 = UE default."));
		}
		ImGui::TreePop();
	}
	ImGui::EndDisabled();
}

void ExponentialHeightFog::SetupResources()
{
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	Util::SetResourceName(linearSampler.get(), "ExponentialHeightFog::LinearSampler");

	samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, shadowSampler.put()));
	Util::SetResourceName(shadowSampler.get(), "ExponentialHeightFog::ShadowSampler");

	volumetricFogCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumetricFogCB>(), "ExponentialHeightFog::VolumetricFogCB");
}

void ExponentialHeightFog::ClearShaderCache()
{
	materialSetupCS.Reset();
	farMaterialSetupCS.Reset();
	conservativeDepthCS.Reset();
	farConservativeDepthCS.Reset();
	lightScatteringCS.Reset();
	farLightScatteringCS.Reset();
	integrationCS.Reset();
	farIntegrationCS.Reset();
	hasLightScatteringHistory = false;
	hasLightScatteringFarHistory = false;
}

void ExponentialHeightFog::CaptureDirectionalShadowMap()
{
	ID3D11ShaderResourceView* shadowMap = nullptr;
	globals::d3d::context->PSGetShaderResources(4, 1, &shadowMap);
	directionalShadowMap.copy_from(shadowMap);
	if (shadowMap)
		shadowMap->Release();
}

void ExponentialHeightFog::EnsureVolumetricResources()
{
	uint32_t pixelSize = std::clamp(settings.volumetricGridPixelSize, 4u, 64u);
	const uint32_t gridZ = std::clamp(settings.volumetricGridSizeZ, 16u, 160u);
	uint32_t farPixelSize = std::clamp(settings.volumetricFarGridPixelSize, 4u, 64u);
	const uint32_t farGridZ = std::clamp(settings.volumetricFarGridSizeZ, 16u, 160u);
	auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);

	auto getGridSize = [&renderSize](uint32_t a_pixelSize, uint32_t a_gridZ) {
		auto gridSize = DirectX::XMUINT4{
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.x / static_cast<float>(a_pixelSize)))),
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.y / static_cast<float>(a_pixelSize)))),
			a_gridZ,
			0u
		};
		if (globals::game::isVR)
			gridSize.x = (gridSize.x + 1u) & ~1u;
		return gridSize;
	};
	DirectX::XMUINT4 gridSize = getGridSize(pixelSize, gridZ);

	constexpr uint64_t maxVolumeVoxels = 16ull * 1024ull * 1024ull;
	while (pixelSize < 64u &&
		   static_cast<uint64_t>(gridSize.x) * gridSize.y * gridSize.z > maxVolumeVoxels) {
		pixelSize++;
		gridSize = getGridSize(pixelSize, gridZ);
	}

	// The far volume must be coarser than the near volume.
	farPixelSize = std::max(farPixelSize, pixelSize);
	DirectX::XMUINT4 farGridSize = getGridSize(farPixelSize, farGridZ);
	while (farPixelSize < 64u &&
		   static_cast<uint64_t>(farGridSize.x) * farGridSize.y * farGridSize.z > maxVolumeVoxels / 4ull) {
		farPixelSize++;
		farGridSize = getGridSize(farPixelSize, farGridZ);
	}

	if (vBufferA &&
		currentGridSize.x == gridSize.x && currentGridSize.y == gridSize.y && currentGridSize.z == gridSize.z &&
		currentFarGridSize.x == farGridSize.x && currentFarGridSize.y == farGridSize.y && currentFarGridSize.z == farGridSize.z)
		return;

	currentGridSize = gridSize;
	currentFarGridSize = farGridSize;

	auto make3D = [this](const DirectX::XMUINT4& a_size, const char* a_name) {
		D3D11_TEXTURE3D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.Depth = a_size.z;
		texDesc.MipLevels = 1;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
		uavDesc.Texture3D.MipSlice = 0;
		uavDesc.Texture3D.FirstWSlice = 0;
		uavDesc.Texture3D.WSize = a_size.z;

		auto tex = std::make_unique<Texture3D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	auto make2D = [this](const DirectX::XMUINT4& a_size, const char* a_name) {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		auto tex = std::make_unique<Texture2D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	vBufferA = make3D(gridSize, "ExponentialHeightFog::VBufferA");
	conservativeDepth = make2D(gridSize, "ExponentialHeightFog::ConservativeDepth");
	lightScattering = make3D(gridSize, "ExponentialHeightFog::LightScattering");
	integratedLightScattering = make3D(gridSize, "ExponentialHeightFog::IntegratedLightScattering");

	conservativeDepthHistory = std::make_unique<Texture2D>(conservativeDepth->desc, "ExponentialHeightFog::ConservativeDepthHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = conservativeDepth->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		conservativeDepthHistory->CreateSRV(srvDesc);
	}

	lightScatteringHistory = std::make_unique<Texture3D>(lightScattering->desc, "ExponentialHeightFog::LightScatteringHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = lightScattering->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
		lightScatteringHistory->CreateSRV(srvDesc);
	}

	vBufferAFar = make3D(farGridSize, "ExponentialHeightFog::VBufferAFar");
	conservativeDepthFar = make2D(farGridSize, "ExponentialHeightFog::ConservativeDepthFar");
	lightScatteringFar = make3D(farGridSize, "ExponentialHeightFog::LightScatteringFar");
	integratedLightScatteringFar = make3D(farGridSize, "ExponentialHeightFog::IntegratedLightScatteringFar");

	conservativeDepthFarHistory = std::make_unique<Texture2D>(conservativeDepthFar->desc, "ExponentialHeightFog::ConservativeDepthFarHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = conservativeDepthFar->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		conservativeDepthFarHistory->CreateSRV(srvDesc);
	}

	lightScatteringFarHistory = std::make_unique<Texture3D>(lightScatteringFar->desc, "ExponentialHeightFog::LightScatteringFarHistory");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = lightScatteringFar->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
		lightScatteringFarHistory->CreateSRV(srvDesc);
	}

	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
}

void ExponentialHeightFog::ReleaseVolumetricResources()
{
	vBufferA.reset();
	vBufferAFar.reset();
	conservativeDepth.reset();
	conservativeDepthHistory.reset();
	conservativeDepthFar.reset();
	conservativeDepthFarHistory.reset();
	lightScattering.reset();
	lightScatteringHistory.reset();
	lightScatteringFar.reset();
	lightScatteringFarHistory.reset();
	integratedLightScattering.reset();
	integratedLightScatteringFar.reset();
	currentGridSize = {};
	currentFarGridSize = {};
	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &nullSRV);
	globals::d3d::context->PSSetShaderResources(22, 1, &nullSRV);
}

void ExponentialHeightFog::BindIntegratedLightScattering()
{
	ID3D11ShaderResourceView* srv = integratedLightScattering ? integratedLightScattering->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &srv);
	ID3D11ShaderResourceView* farSrv = integratedLightScatteringFar ? integratedLightScatteringFar->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(22, 1, &farSrv);
}

ID3D11ComputeShader* ExponentialHeightFog::GetMaterialSetupCS()
{
	return materialSetupCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", {}, "cs_5_0");
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarMaterialSetupCS()
{
	return farMaterialSetupCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0");
}

ID3D11ComputeShader* ExponentialHeightFog::GetConservativeDepthCS()
{
	return conservativeDepthCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", {}, "cs_5_0");
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarConservativeDepthCS()
{
	return farConservativeDepthCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0");
}

ID3D11ComputeShader* ExponentialHeightFog::GetLightScatteringCS()
{
	return lightScatteringCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", LightScatteringDefines(false), "cs_5_0");
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarLightScatteringCS()
{
	return farLightScatteringCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", LightScatteringDefines(true), "cs_5_0");
}

ID3D11ComputeShader* ExponentialHeightFog::GetIntegrationCS()
{
	return integrationCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", {}, "cs_5_0");
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarIntegrationCS()
{
	return farIntegrationCS.Get(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0");
}

void ExponentialHeightFog::Prepass()
{
	CS_GPU_PASS("ExponentialHeightFog::Prepass");
	if (!settings.enabled || globals::state->isMapMenuOpen || !settings.volumetricFogEnabled || settings.volumetricFogExtinctionScale <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}

	const auto cameraData = Util::GetCameraData();
	const float volumeStart = settings.useVanillaFogSettings ? 0.0f : std::max(settings.volumetricFogStartDistance, 0.0f);
	if (settings.volumetricFogDistance <= std::max(cameraData.y, volumeStart) + 10.0f) {
		ReleaseVolumetricResources();
		return;
	}
	const Settings frameSettings = GetCommonBufferData();
	const float fogDensity = frameSettings.useVanillaFogSettings ? frameSettings.vanillaFogDensity * frameSettings.vanillaFogStrength : frameSettings.fogDensity;
	if (fogDensity <= 0.0f && frameSettings.fogDensity2 <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}
	EnsureVolumetricResources();
	if (lastPrepassFrame == UINT32_MAX || !CanReuseFogHistory(frameSettings, previousFogSettings)) {
		hasLightScatteringHistory = false;
		hasLightScatteringFarHistory = false;
	}
	previousFogSettings = frameSettings;

	ID3D11ShaderResourceView* directionalShadowLightData = globals::deferred && globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool hasLocalLightData =
		lightLimitFix.loaded &&
		lightLimitFix.lights &&
		lightLimitFix.lightIndexList &&
		lightLimitFix.lightGrid;
	auto* depthSrv = Util::GetCurrentSceneDepthSRV(true);
	auto& ibl = globals::features::ibl;
	auto& skylighting = globals::features::skylighting;
	const bool hasIBL = ibl.loaded &&
	                    ibl.settings.EnableIBL != 0 &&
	                    !ibl.IsDisabledForCurrentScene() &&
	                    ibl.envIBLTexture &&
	                    ibl.skyIBLTexture;
	const bool hasSkylighting = skylighting.loaded && skylighting.texProbeArray;

	const auto linearLightingData = globals::features::linearLighting.GetCommonBufferData();
	const std::array currentColorSpace{ linearLightingData.enableLinearLighting, linearLightingData.enableACEScg };
	const bool temporalReprojection = Util::GetTemporal();
	const bool temporalHistoryValid =
		temporalReprojection &&
		hasLightScatteringHistory &&
		historyColorSpace == currentColorSpace &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;
	const bool temporalHistoryValidFar =
		temporalReprojection &&
		hasLightScatteringFarHistory &&
		historyColorSpace == currentColorSpace &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;

	const double nearPlane = std::max(static_cast<double>(cameraData.y), static_cast<double>(volumeStart));
	const double totalFarPlane = std::max(nearPlane + 1.0, static_cast<double>(settings.volumetricFogDistance));
	const double nearEndDepth = std::min(
		std::max(static_cast<double>(std::max(settings.volumetricNearGridDistance, 0.0f)), nearPlane + 1.0),
		totalFarPlane);
	const bool farGridEnabled = nearEndDepth + 1.0 < totalFarPlane;

	auto computeGridZParams = [](double a_nearPlane, double a_farPlane, uint32_t a_gridZ, float a_distributionScale) {
		const double nearWithOffset = a_nearPlane + 0.095 * 100.0;
		const double depthDistributionScale = std::max(static_cast<double>(a_distributionScale), static_cast<double>(a_gridZ) / 120.0);
		const double farExp = std::exp2(std::min(static_cast<double>(a_gridZ) / depthDistributionScale, 120.0));
		const double gridZOffset = (a_farPlane - nearWithOffset * farExp) / (a_farPlane - nearWithOffset);
		const double gridZScale = (1.0 - gridZOffset) / nearWithOffset;
		return float4{
			static_cast<float>(gridZScale),
			static_cast<float>(gridZOffset),
			static_cast<float>(depthDistributionScale),
			0.0f
		};
	};

	const float nearFadeInvDistance = !settings.useVanillaFogSettings && settings.volumetricFogNearFadeInDistance > 0.0f ? 1.0f / settings.volumetricFogNearFadeInDistance : 100000000.0f;

	VolumetricFogCB cb{};
	cb.gridSizeAndFlags = {
		currentGridSize.x,
		currentGridSize.y,
		currentGridSize.z,
		(directionalShadowMap && directionalShadowLightData ? 1u : 0u) |
			(depthSrv ? 2u : 0u) |
			(hasIBL ? 4u : 0u) |
			(hasSkylighting ? 8u : 0u) |
			(depthSrv && temporalHistoryValid && hasConservativeDepthHistory ? 16u : 0u) |
			(hasLocalLightData ? 32u : 0u)
	};
	cb.invGridSizeAndNearFade = float4{
		1.0f / static_cast<float>(currentGridSize.x),
		1.0f / static_cast<float>(currentGridSize.y),
		1.0f / static_cast<float>(currentGridSize.z),
		nearFadeInvDistance
	};
	cb.gridZParams = computeGridZParams(nearPlane, nearEndDepth, currentGridSize.z, settings.volumetricDepthDistributionScale);

	cb.farGridSizeAndFlags = {
		currentFarGridSize.x,
		currentFarGridSize.y,
		currentFarGridSize.z,
		(directionalShadowMap && directionalShadowLightData ? 1u : 0u) |
			(depthSrv ? 2u : 0u) |
			(hasIBL ? 4u : 0u) |
			(hasSkylighting ? 8u : 0u) |
			(depthSrv && temporalHistoryValidFar && hasConservativeDepthFarHistory ? 16u : 0u)
	};
	cb.farInvGridSizeAndNearFade = float4{
		1.0f / static_cast<float>(currentFarGridSize.x),
		1.0f / static_cast<float>(currentFarGridSize.y),
		1.0f / static_cast<float>(currentFarGridSize.z),
		nearFadeInvDistance
	};
	cb.farGridZParams = computeGridZParams(nearEndDepth, totalFarPlane, currentFarGridSize.z, settings.volumetricDepthDistributionScale);
	cb.farRange = float4{ static_cast<float>(nearEndDepth), static_cast<float>(totalFarPlane), 0.0f, 0.0f };

	const uint32_t eyeCount = globals::game::isVR ? 2u : 1u;
	for (uint32_t eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
		cb.clipToWorld[eyeIndex] = globals::game::frameBufferCached.GetCameraViewProjUnjittered(eyeIndex).Invert();
	}
	if (eyeCount == 1u) {
		cb.clipToWorld[1] = cb.clipToWorld[0];
	}

	for (uint32_t i = 0; i < std::size(cb.frameJitterOffsets); i++) {
		const uint32_t temporalFrame = (globals::state->frameCount - i) & 1023u;
		cb.frameJitterOffsets[i] = float4{
			temporalReprojection ? Halton(temporalFrame, 2) : 0.5f,
			temporalReprojection ? Halton(temporalFrame, 3) : 0.5f,
			temporalReprojection ? Halton(temporalFrame, 5) : 0.5f,
			0.0f
		};
	}
	cb.historyParameters = float4{
		temporalHistoryValid ? std::clamp(settings.volumetricHistoryWeight, 0.0f, 0.99f) : 0.0f,
		static_cast<float>(std::clamp(settings.volumetricHistoryMissSampleCount, 1u, 16u)),
		0.0f,
		0.0f
	};
	cb.jitterParameters = float4{
		temporalReprojection ? std::max(settings.volumetricSampleJitterMultiplier, 0.0f) : 0.0f,
		static_cast<float>(globals::state->frameCount % 8u),
		0.0f,
		0.0f
	};
	volumetricFogCB->Update(cb);

	auto context = globals::d3d::context;
	ID3D11Buffer* cbuffers[1]{ volumetricFogCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbuffers);

	globals::state->BindSharedDataCS(context);

	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);

	ID3D11SamplerState* samplers[2]{ linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);

	context->CSSetShaderResources(17, 1, &depthSrv);
	ID3D11ShaderResourceView* skylightingSrv = hasSkylighting ? skylighting.texProbeArray->srv.get() : nullptr;
	ID3D11ShaderResourceView* iblSrvs[2]{
		hasIBL ? ibl.envIBLTexture->srv.get() : nullptr,
		hasIBL ? ibl.skyIBLTexture->srv.get() : nullptr
	};
	context->CSSetShaderResources(50, 1, &skylightingSrv);
	context->CSSetShaderResources(76, 2, iblSrvs);

	struct VolumetricPassDesc
	{
		DirectX::XMUINT4 gridSize;
		Texture3D* vBuffer;
		Texture2D* conservativeDepth;
		Texture2D* conservativeDepthHistory;  // may be null
		Texture3D* scattering;
		Texture3D* scatteringHistory;  // may be null
		Texture3D* integrated;
		ID3D11ComputeShader* materialSetupCS;
		ID3D11ComputeShader* conservativeDepthCS;
		ID3D11ComputeShader* lightScatteringCS;
		ID3D11ComputeShader* integrationCS;
		bool hasPrevConservativeDepth;
		bool isFar;
	};

	// A stage's LazyShader can be permanently unavailable after a failed compile.
	// Track whether every required stage actually dispatched so a skipped stage's
	// stale or uninitialized output isn't published to history or bound downstream.
	bool allStagesOk = true;

	auto runVolumetricPass = [&](const VolumetricPassDesc& p) {
		CS_GPU_PASS_SELECT(p.isFar, "ExponentialHeightFog::FarVolume", "ExponentialHeightFog::NearVolume");
		const uint32_t groupX = (p.gridSize.x + 7) / 8;
		const uint32_t groupY = (p.gridSize.y + 7) / 8;
		const uint32_t groupZ = (p.gridSize.z + 3) / 4;

		if (depthSrv) {
			ID3D11UnorderedAccessView* uavs[1]{ p.conservativeDepth->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			if (p.conservativeDepthCS) {
				context->CSSetShader(p.conservativeDepthCS, nullptr, 0);
				context->Dispatch(groupX, groupY, 1);
			} else {
				allStagesOk = false;
			}
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11UnorderedAccessView* uavs[1]{ p.vBuffer->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			if (p.materialSetupCS) {
				context->CSSetShader(p.materialSetupCS, nullptr, 0);
				context->Dispatch(groupX, groupY, groupZ);
			} else {
				allStagesOk = false;
			}
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[5]{
				p.vBuffer->srv.get(),
				directionalShadowMap.get(),
				p.scatteringHistory ? p.scatteringHistory->srv.get() : nullptr,
				p.conservativeDepth->srv.get(),
				p.hasPrevConservativeDepth && p.conservativeDepthHistory ? p.conservativeDepthHistory->srv.get() : nullptr
			};
			ID3D11ShaderResourceView* localLightSrvs[3]{
				hasLocalLightData ? lightLimitFix.lights->srv.get() : nullptr,
				hasLocalLightData ? lightLimitFix.lightIndexList->srv.get() : nullptr,
				hasLocalLightData ? lightLimitFix.lightGrid->srv.get() : nullptr
			};
			ID3D11UnorderedAccessView* uavs[1]{ p.scattering->uav.get() };
			context->CSSetShaderResources(0, 5, srvs);
			context->CSSetShaderResources(35, 3, localLightSrvs);
			context->CSSetShaderResources(98, 1, &directionalShadowLightData);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			if (p.lightScatteringCS) {
				context->CSSetShader(p.lightScatteringCS, nullptr, 0);
				context->Dispatch(groupX, groupY, groupZ);
			} else {
				allStagesOk = false;
			}
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[1]{ p.scattering->srv.get() };
			ID3D11UnorderedAccessView* uavs[1]{ p.integrated->uav.get() };
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			if (p.integrationCS) {
				context->CSSetShader(p.integrationCS, nullptr, 0);
				context->Dispatch(groupX, groupY, 1);
			} else {
				allStagesOk = false;
			}
		}
	};

	runVolumetricPass({ currentGridSize,
		vBufferA.get(),
		conservativeDepth.get(),
		temporalHistoryValid && hasConservativeDepthHistory ? conservativeDepthHistory.get() : nullptr,
		lightScattering.get(),
		temporalHistoryValid ? lightScatteringHistory.get() : nullptr,
		integratedLightScattering.get(),
		GetMaterialSetupCS(),
		GetConservativeDepthCS(),
		GetLightScatteringCS(),
		GetIntegrationCS(),
		temporalHistoryValid && hasConservativeDepthHistory,
		false });

	if (farGridEnabled) {
		runVolumetricPass({ currentFarGridSize,
			vBufferAFar.get(),
			conservativeDepthFar.get(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory ? conservativeDepthFarHistory.get() : nullptr,
			lightScatteringFar.get(),
			temporalHistoryValidFar ? lightScatteringFarHistory.get() : nullptr,
			integratedLightScatteringFar.get(),
			GetFarMaterialSetupCS(),
			GetFarConservativeDepthCS(),
			GetFarLightScatteringCS(),
			GetFarIntegrationCS(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory,
			true });
	} else {
		const float clearValue[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		context->ClearUnorderedAccessViewFloat(integratedLightScatteringFar->uav.get(), clearValue);
	}

	ID3D11ShaderResourceView* nullSrvs[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11ShaderResourceView* nullDepthSrv[1]{ nullptr };
	ID3D11UnorderedAccessView* nullUav[1]{ nullptr };
	ID3D11SamplerState* nullSamplers[2]{ nullptr, nullptr };
	ID3D11Buffer* nullCb[1]{ nullptr };
	context->CSSetShaderResources(0, 5, nullSrvs);
	context->CSSetShaderResources(17, 1, nullDepthSrv);
	context->CSSetShaderResources(35, 3, nullSrvs);
	context->CSSetShaderResources(50, 1, nullDepthSrv);
	context->CSSetShaderResources(76, 2, nullSrvs);
	context->CSSetShaderResources(98, 1, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	if (temporalReprojection && allStagesOk) {
		context->CopyResource(lightScatteringHistory->resource.get(), lightScattering->resource.get());
		hasLightScatteringHistory = true;
		historyColorSpace = currentColorSpace;
		if (depthSrv) {
			context->CopyResource(conservativeDepthHistory->resource.get(), conservativeDepth->resource.get());
			hasConservativeDepthHistory = true;
		} else {
			hasConservativeDepthHistory = false;
		}
		if (farGridEnabled) {
			context->CopyResource(lightScatteringFarHistory->resource.get(), lightScatteringFar->resource.get());
			hasLightScatteringFarHistory = true;
			if (depthSrv) {
				context->CopyResource(conservativeDepthFarHistory->resource.get(), conservativeDepthFar->resource.get());
				hasConservativeDepthFarHistory = true;
			} else {
				hasConservativeDepthFarHistory = false;
			}
		} else {
			hasLightScatteringFarHistory = false;
			hasConservativeDepthFarHistory = false;
		}
	} else {
		// A skipped stage left lightScattering/conservativeDepth stale or
		// uninitialized this frame -- don't let a future frame reproject from it.
		hasLightScatteringHistory = false;
		hasConservativeDepthHistory = false;
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
	}

	lastPrepassFrame = globals::state->frameCount;
	if (allStagesOk) {
		BindIntegratedLightScattering();
	} else {
		ID3D11ShaderResourceView* nullSrv = nullptr;
		context->PSSetShaderResources(19, 1, &nullSrv);
		context->PSSetShaderResources(22, 1, &nullSrv);
	}
}

#undef I18N_KEY_PREFIX
