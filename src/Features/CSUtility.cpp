#include "CSUtility.h"

#include "Bloom.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "LightLimitFix.h"
#include "LinearLighting.h"
#include "State.h"
#include "TreeWindPatcher.h"
#include "UnderwaterDepthOfField.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Format.h"
#include "Utils/Game.h"
#include "Utils/PointLightFlags.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>

#define I18N_KEY_PREFIX "feature.cs_utility."

namespace
{
	constexpr float kSkyBrightnessMin = 0.0f;
	constexpr float kSkyBrightnessMax = 2.0f;
	constexpr float kMultiplierMin = 0.0f;
	constexpr float kMultiplierMax = 5.0f;
	constexpr float kTrunkWindIntensityMin = 0.0f;
	constexpr float kTrunkWindIntensityMax = 10.0f;
	constexpr float kTrunkWindFlexibleHeightMin = 1.0f;
	constexpr float kTrunkWindFlexibleHeightMax = 16384.0f;
	constexpr float kTrunkWindMaximumDisplacementMin = 0.0f;
	constexpr float kTrunkWindMaximumDisplacementMax = 4096.0f;
	constexpr float kTrunkWindSensitivityMin = 0.0f;
	constexpr float kTrunkWindSensitivityMax = 20.0f;
	constexpr float kTreeLeafAmbientSensitivityMin = 0.0f;
	constexpr float kTreeLeafAmbientSensitivityMax = 4.0f;
	constexpr float kWindFieldGustScaleMin = 128.0f;
	constexpr float kWindFieldGustScaleMax = 16384.0f;
	constexpr float kWindFieldGustAmplitudeMin = 0.0f;
	constexpr float kWindFieldGustAmplitudeMax = 1.0f;
	constexpr float kWindFieldGustAdvectionMultiplierMin = 0.0f;
	constexpr float kWindFieldGustAdvectionMultiplierMax = 8.0f;
	constexpr float kGrassWindResponseMin = 0.0f;
	constexpr float kGrassWindResponseMax = 180.0f;
	constexpr float kGrassWindMaximumTiltMin = 0.0f;
	constexpr float kGrassWindMaximumTiltMax = 89.0f;
	constexpr float kGrassWindBendProfileMin = 0.0f;
	constexpr float kGrassWindBendProfileMax = 1.0f;
	constexpr float kGrassWindSpringLagMin = 0.0f;
	constexpr float kGrassWindSpringLagMax = 0.5f;
	constexpr float kGrassWindSpringStrengthMin = 0.0f;
	constexpr float kGrassWindSpringStrengthMax = 1.0f;
	constexpr float kGrassWindSpringRecoveryMin = 0.0f;
	constexpr float kGrassWindSpringRecoveryMax = 0.5f;
	constexpr float kGrassWindFlutterStrengthMin = 0.0f;
	constexpr float kGrassWindFlutterStrengthMax = 2.0f;
	constexpr float kGrassWindFlutterFrequencyMin = 0.0f;
	constexpr float kGrassWindFlutterFrequencyMax = 2.0f;
	constexpr float kWaterBrightnessMin = 0.0f;
	constexpr float kWaterBrightnessMax = 2.0f;
	constexpr float kWaterAmountMin = 0.0f;
	constexpr float kWaterAmountMax = 2.0f;
	constexpr float kWaterSunSpecularMax = 5.0f;
	constexpr float kWaterFresnelMin = 0.0f;
	constexpr float kWaterFresnelMax = 1.0f;
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
		a_settings.trunkWindIntensityOverride = ClampFiniteOrDefault(a_settings.trunkWindIntensityOverride, kTrunkWindIntensityMin, kTrunkWindIntensityMax, defaults.trunkWindIntensityOverride);
		a_settings.trunkWindFlexibleHeight = ClampFiniteOrDefault(a_settings.trunkWindFlexibleHeight, kTrunkWindFlexibleHeightMin, kTrunkWindFlexibleHeightMax, defaults.trunkWindFlexibleHeight);
		a_settings.trunkWindMaximumDisplacement = ClampFiniteOrDefault(a_settings.trunkWindMaximumDisplacement, kTrunkWindMaximumDisplacementMin, kTrunkWindMaximumDisplacementMax, defaults.trunkWindMaximumDisplacement);
		a_settings.trunkWindBendSensitivity = ClampFiniteOrDefault(a_settings.trunkWindBendSensitivity, kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, defaults.trunkWindBendSensitivity);
		a_settings.treeLeafAmbientSensitivity = ClampFiniteOrDefault(a_settings.treeLeafAmbientSensitivity, kTreeLeafAmbientSensitivityMin, kTreeLeafAmbientSensitivityMax, defaults.treeLeafAmbientSensitivity);
		a_settings.windFieldGustScale = ClampFiniteOrDefault(a_settings.windFieldGustScale, kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
		a_settings.windFieldGustAmplitude = ClampFiniteOrDefault(a_settings.windFieldGustAmplitude, kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, defaults.windFieldGustAmplitude);
		a_settings.windFieldGustAdvectionMultiplier = ClampFiniteOrDefault(a_settings.windFieldGustAdvectionMultiplier, kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, defaults.windFieldGustAdvectionMultiplier);
		a_settings.grassWindResponse = ClampFiniteOrDefault(a_settings.grassWindResponse, kGrassWindResponseMin, kGrassWindResponseMax, defaults.grassWindResponse);
		a_settings.grassWindMaximumTilt = ClampFiniteOrDefault(a_settings.grassWindMaximumTilt, kGrassWindMaximumTiltMin, kGrassWindMaximumTiltMax, defaults.grassWindMaximumTilt);
		a_settings.grassWindBendProfile = ClampFiniteOrDefault(a_settings.grassWindBendProfile, kGrassWindBendProfileMin, kGrassWindBendProfileMax, defaults.grassWindBendProfile);
		a_settings.grassWindSpringLag = ClampFiniteOrDefault(a_settings.grassWindSpringLag, kGrassWindSpringLagMin, kGrassWindSpringLagMax, defaults.grassWindSpringLag);
		a_settings.grassWindSpringStrength = ClampFiniteOrDefault(a_settings.grassWindSpringStrength, kGrassWindSpringStrengthMin, kGrassWindSpringStrengthMax, defaults.grassWindSpringStrength);
		a_settings.grassWindSpringRecovery = ClampFiniteOrDefault(a_settings.grassWindSpringRecovery, kGrassWindSpringRecoveryMin, kGrassWindSpringRecoveryMax, defaults.grassWindSpringRecovery);
		a_settings.grassWindFlutterStrength = ClampFiniteOrDefault(a_settings.grassWindFlutterStrength, kGrassWindFlutterStrengthMin, kGrassWindFlutterStrengthMax, defaults.grassWindFlutterStrength);
		a_settings.grassWindFlutterFrequency = ClampFiniteOrDefault(a_settings.grassWindFlutterFrequency, kGrassWindFlutterFrequencyMin, kGrassWindFlutterFrequencyMax, defaults.grassWindFlutterFrequency);
		a_settings.skyBrightness = ClampFiniteOrDefault(a_settings.skyBrightness, kSkyBrightnessMin, kSkyBrightnessMax, defaults.skyBrightness);
		a_settings.directionalLightMult = ClampFiniteOrDefault(a_settings.directionalLightMult, kMultiplierMin, kMultiplierMax, defaults.directionalLightMult);
		a_settings.pointLightMult = ClampFiniteOrDefault(a_settings.pointLightMult, kMultiplierMin, kMultiplierMax, defaults.pointLightMult);
		a_settings.linearPointLightMult = ClampFiniteOrDefault(a_settings.linearPointLightMult, kMultiplierMin, kMultiplierMax, defaults.linearPointLightMult);
		a_settings.spotlightMult = ClampFiniteOrDefault(a_settings.spotlightMult, kMultiplierMin, kMultiplierMax, defaults.spotlightMult);
		a_settings.linearSpotlightMult = ClampFiniteOrDefault(a_settings.linearSpotlightMult, kMultiplierMin, kMultiplierMax, defaults.linearSpotlightMult);
		a_settings.omnidirectionalBulbMult = ClampFiniteOrDefault(a_settings.omnidirectionalBulbMult, kMultiplierMin, kMultiplierMax, defaults.omnidirectionalBulbMult);
		a_settings.linearOmnidirectionalBulbMult = ClampFiniteOrDefault(a_settings.linearOmnidirectionalBulbMult, kMultiplierMin, kMultiplierMax, defaults.linearOmnidirectionalBulbMult);
		CSUtility::SanitizeWaterSettings(a_settings.water);
		CSUtility::SanitizeDepthOfFieldOverride(a_settings.sceneDof);
		CSUtility::SanitizeDepthOfFieldOverride(a_settings.underwaterDof);
		Bloom::SanitizeSettings(a_settings.bloomEnhancement);
	}

	void ResetGrassWindSettings(CSUtility::Settings& a_settings)
	{
		const CSUtility::Settings defaults{};
		a_settings.overrideTrunkWindIntensity = defaults.overrideTrunkWindIntensity;
		a_settings.trunkWindIntensityOverride = defaults.trunkWindIntensityOverride;
		a_settings.enableAmbientGrassWind = defaults.enableAmbientGrassWind;
		a_settings.grassWindResponse = defaults.grassWindResponse;
		a_settings.grassWindMaximumTilt = defaults.grassWindMaximumTilt;
		a_settings.grassWindBendProfile = defaults.grassWindBendProfile;
		a_settings.grassWindUseBendTargetSpring = defaults.grassWindUseBendTargetSpring;
		a_settings.grassWindSpringLag = defaults.grassWindSpringLag;
		a_settings.grassWindSpringStrength = defaults.grassWindSpringStrength;
		a_settings.grassWindSpringRecovery = defaults.grassWindSpringRecovery;
		a_settings.grassWindFlutterStrength = defaults.grassWindFlutterStrength;
		a_settings.grassWindFlutterFrequency = defaults.grassWindFlutterFrequency;
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

	std::string CompactMeshPath(std::string_view a_path, float a_availableWidth)
	{
		const std::string fullPath(a_path);
		if (ImGui::CalcTextSize(fullPath.c_str()).x <= a_availableWidth)
			return fullPath;

		const auto fileSeparator = a_path.rfind('/');
		if (fileSeparator == std::string_view::npos)
			return fullPath;

		const std::string fileName(a_path.substr(fileSeparator + 1));
		if (ImGui::CalcTextSize(fileName.c_str()).x > a_availableWidth)
			return fileName;

		std::string compactPath = ".../" + fileName;
		auto suffixStart = fileSeparator;
		while (suffixStart > 0) {
			const auto previousSeparator = a_path.rfind('/', suffixStart - 1);
			if (previousSeparator == std::string_view::npos)
				break;
			const std::string candidate = "..." + std::string(a_path.substr(previousSeparator));
			if (ImGui::CalcTextSize(candidate.c_str()).x > a_availableWidth)
				break;
			compactPath = candidate;
			suffixStart = previousSeparator;
		}
		return compactPath;
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
	CSUtility::Settings,
	enableTrunkBend,
	overrideTrunkWindIntensity,
	trunkWindIntensityOverride,
	trunkWindFlexibleHeight,
	trunkWindMaximumDisplacement,
	trunkWindBendSensitivity,
	treeLeafAmbientSensitivity,
	windFieldGustScale,
	windFieldGustAmplitude,
	windFieldGustAdvectionMultiplier,
	enableAmbientGrassWind,
	grassWindResponse,
	grassWindMaximumTilt,
	grassWindBendProfile,
	grassWindUseBendTargetSpring,
	grassWindSpringLag,
	grassWindSpringStrength,
	grassWindSpringRecovery,
	grassWindFlutterStrength,
	grassWindFlutterFrequency,
	skyBrightness,
	directionalLightMult,
	pointLightMult,
	linearPointLightMult,
	spotlightMult,
	linearSpotlightMult,
	omnidirectionalBulbMult,
	linearOmnidirectionalBulbMult,
	water,
	sceneDof,
	underwaterDof,
	bloomEnhancement)

void CSUtility::DrawSettings()
{
	const auto drawWindField = [&] {
		ImGui::Checkbox(T(TKEY("visualize_wind_field"), "Visualize Wind Field"), &visualizeWindField);
		ImGui::TextWrapped("%s", T(TKEY("visualize_wind_field_tooltip"),
									 "Colors visible world geometry by GPU-sampled ambient gust pressure."));
		ImGui::Checkbox(T(TKEY("wind_field_use_real_speed"), "Wind Debug: Use Real Wind Speed"), &windFieldUseRealSpeed);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("wind_field_use_real_speed_tooltip"),
				"Use the current weather wind magnitude for air velocity and the base gust-front travel rate; otherwise use the override below."));
		}
		ImGui::SliderFloat(T(TKEY("wind_field_override_speed"), "Wind Debug: Override Speed"), &windFieldOverrideSpeed,
			0.0f, 2.0f, "%.3f");
		if (treeWindTest.enabled) {
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
				T(TKEY("tree_wind_test_active_notice"), "Tree Meshes runtime test conditions currently override wind speed and gust tuning."));
		}
		ImGui::Checkbox(T(TKEY("wind_field_use_real_direction"), "Wind Debug: Use Real Wind Direction"), &windFieldUseRealDirection);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("wind_field_use_real_direction_tooltip"),
				"Use the current weather wind direction for the displayed field; otherwise use hardcoded world-space +X."));
		}
		ImGui::SeparatorText(T(TKEY("wind_field_live_values"), "Wind Field Live Values"));
		const auto* state = globals::state;
		const auto* treeManager = RE::BSTreeManager::GetSingleton();
		const auto* sky = globals::game::sky;
		const auto* weather = sky ? sky->currentWeather : nullptr;
		const float ambientDirectionLength = std::hypot(state->ambientWindVelocity.x, state->ambientWindVelocity.y);
		const float ambientDirectionDegrees = ambientDirectionLength > 0.0001f ?
		                                          std::atan2(state->ambientWindVelocity.y, state->ambientWindVelocity.x) * (180.0f / 3.14159265358979323846f) :
		                                          0.0f;
		ImGui::Text("%s: (%.5f, %.5f, %.5f)", T(TKEY("wind_field_ambient_velocity"), "Ambient velocity"),
			state->ambientWindVelocity.x, state->ambientWindVelocity.y, state->ambientWindVelocity.z);
		ImGui::Text("%s: %.5f", T(TKEY("wind_field_ambient_speed"), "Selected ambient speed"), state->windFieldAmbientSpeed);
		ImGui::Text("%s: %.2f deg", T(TKEY("wind_field_ambient_direction"), "Ambient direction"), ambientDirectionDegrees);
		ImGui::Text("%s: %.6f s", T(TKEY("wind_field_frame_time"), "Frame delta"), state->windFieldFrameTime);
		ImGui::Text("%s: %.6f", T(TKEY("wind_field_travel_delta"), "Travel delta"), state->windFieldTravelDelta);
		ImGui::Text("%s: %.5f", T(TKEY("wind_field_travel_distance"), "Accumulated travel distance"), state->windFieldGustTravelDistance);
		ImGui::Text("%s: %.3f units/s", T(TKEY("wind_field_advection_speed"), "Gust advection speed"), state->windFieldAdvectionSpeed);
		ImGui::Text("%s: %.5f", T(TKEY("wind_field_global_time"), "Global timer"), state->timer);
		if (treeManager) {
			ImGui::Text("%s: (%.5f, %.5f), magnitude %.5f", T(TKEY("wind_field_tree_input"), "Tree wind input"),
				treeManager->windDirection.x, treeManager->windDirection.y, treeManager->windMagnitude);
		}
		if (sky)
			ImGui::Text("%s: %.5f", T(TKEY("wind_field_sky_speed"), "Sky wind speed"), sky->windSpeed);
		if (weather)
			ImGui::Text("%s: %.3f (raw %u), direction raw %u", T(TKEY("wind_field_weather_input"), "Weather input"),
				static_cast<unsigned>(weather->data.windSpeed) / 255.0f, static_cast<unsigned>(weather->data.windSpeed),
				static_cast<unsigned>(weather->data.windDirection));
		const float selectedSpeed = state->windFieldSelectedSpeed;
		const float selectedDirectionX = windFieldUseRealDirection && ambientDirectionLength > 0.0001f ?
		                                     state->ambientWindVelocity.x / ambientDirectionLength :
		                                     1.0f;
		const float selectedDirectionY = windFieldUseRealDirection && ambientDirectionLength > 0.0001f ?
		                                     state->ambientWindVelocity.y / ambientDirectionLength :
		                                     0.0f;
		const float weatherWindSpeed = std::sqrt(
			state->ambientWindVelocity.x * state->ambientWindVelocity.x +
			state->ambientWindVelocity.y * state->ambientWindVelocity.y +
			state->ambientWindVelocity.z * state->ambientWindVelocity.z);
		float localWindSpeed = state->windFieldSelectedSpeed;
		float ambientGust = 0.0f;
		ImGui::Text("%s: speed %.5f, direction (%.5f, %.5f, 0.00000)",
			T(TKEY("wind_field_selected_input"), "Selected sampler input"), selectedSpeed, selectedDirectionX, selectedDirectionY);
		ImGui::TextWrapped("%s", T(TKEY("wind_field_color_note"),
									 "The debug color represents normalized ambient gust pressure. Advection controls transport independently from gust amplitude."));
		if (globals::game::shadowState) {
			const auto eyePosition = Util::GetAverageEyePosition();
			const float3 samplePosition{ eyePosition.x, eyePosition.y, eyePosition.z };
			const float rawAmbientSpeed = std::sqrt(
				state->ambientWindVelocity.x * state->ambientWindVelocity.x +
				state->ambientWindVelocity.y * state->ambientWindVelocity.y +
				state->ambientWindVelocity.z * state->ambientWindVelocity.z);
			const auto eyeSample = state->SampleAmbientWind(samplePosition, state->ambientWindVelocity, rawAmbientSpeed);
			const auto selectedSample = state->SampleAmbientWind(samplePosition);
			localWindSpeed = std::sqrt(
				selectedSample.velocity.x * selectedSample.velocity.x +
				selectedSample.velocity.y * selectedSample.velocity.y +
				selectedSample.velocity.z * selectedSample.velocity.z);
			ambientGust = selectedSample.ambientGust;
			ImGui::Text("%s: (%.5f, %.5f, %.5f), gust %.5f",
				T(TKEY("wind_field_cpu_sample"), "CPU sample at camera"), eyeSample.velocity.x, eyeSample.velocity.y,
				eyeSample.velocity.z, eyeSample.ambientGust);
			ImGui::Text("%s: (%.5f, %.5f, %.5f), gust %.5f",
				T(TKEY("wind_field_selected_sample"), "Selected sample at camera"), selectedSample.velocity.x,
				selectedSample.velocity.y, selectedSample.velocity.z, selectedSample.ambientGust);
		}
		ImGui::SeparatorText(T(TKEY("wind_field_readout"), "Field Readout"));
		ImGui::Text("%s: %.2f", T(TKEY("wind_field_weather_wind"), "Weather Wind"), weatherWindSpeed);
		ImGui::Text("%s: %.2f", T(TKEY("wind_field_local_wind"), "Local Wind"), localWindSpeed);
		ImGui::Text("%s: %+.2f", T(TKEY("wind_field_ambient_gust"), "Ambient Gust"), ambientGust);
		ImGui::Text("%s: %.2f units/s", T(TKEY("wind_field_advection_speed_readout"), "Advection Speed"),
			state->windFieldAdvectionSpeed);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_field_readout_tooltip"),
				"Weather Wind is the raw ambient input. Local Wind is the canonical sampled velocity magnitude at the camera. Ambient Gust is the normalized field value used to modulate it."));
		ImGui::SeparatorText(T(TKEY("wind_field_profile"), "Profile"));
		ImGui::Text("%s: %.2f", T(TKEY("wind_field_profile_amplitude"), "Amplitude"), settings.windFieldGustAmplitude);
		ImGui::Text("%s: %.0f", T(TKEY("wind_field_profile_scale"), "Scale"), settings.windFieldGustScale);
		ImGui::Text("%s: %.2fx", T(TKEY("wind_field_profile_advection"), "Advection"), settings.windFieldGustAdvectionMultiplier);
		if (ImGui::TreeNodeEx(T(TKEY("wind_field_tuning"), "Sampler tuning"), ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat(T(TKEY("wind_field_gust_advection_multiplier"), "Gust Advection Multiplier"),
				&settings.windFieldGustAdvectionMultiplier, kWindFieldGustAdvectionMultiplierMin,
				kWindFieldGustAdvectionMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("wind_field_gust_advection_multiplier_tooltip"),
					"Changes only how quickly gust structures move through world space; it does not increase local air velocity."));
			ImGui::SliderFloat(T(TKEY("wind_field_gust_scale"), "Gust Spatial Scale"), &settings.windFieldGustScale,
				kWindFieldGustScaleMin, kWindFieldGustScaleMax, "%.0f units",
				ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("wind_field_gust_scale_tooltip"),
					"Controls the along-wind size of the broad gust structures; the existing detail ratios scale with it."));
			ImGui::SliderFloat(T(TKEY("wind_field_gust_amplitude"), "Gust Amplitude"), &settings.windFieldGustAmplitude,
				kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("wind_field_gust_amplitude_tooltip"),
					"Fractional velocity deviation around the mean; 0.35 produces a 0.65x to 1.35x range."));
			const auto& tuning = state->windFieldTuning;
			ImGui::Text("Base advection %.2f units/s at wind speed 1.0, front aspect %.2f",
				tuning.gustAdvectionBaseSpeed, tuning.frontAspectRatio);
			ImGui::Text("Detail ratios %.3f / %.3f, turbulence %.3f, skew %.3f", tuning.detailScaleRatio,
				tuning.detailCrosswindScaleRatio, tuning.turbulenceStrength, tuning.turbulenceSkew);
			ImGui::Text("Contrast %.3f - %.3f", tuning.contrastLow, tuning.contrastHigh);
			ImGui::Text("Seeds: broad 0x%08X, detail 0x%08X, mix 0x%08X", tuning.broadGustSeed,
				tuning.turbulentGustSeed, tuning.gradientSeedMix);
			ImGui::Text("PCG: multiplier %u, increment %u", tuning.pcgMultiplier, tuning.pcgIncrement);
			ImGui::TreePop();
		}
		ImGui::Separator();
	};

	if (ImGui::BeginTabBar("##CSUtilityTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_wind_field"), "Wind Field"))) {
			activeSettingsPage = SettingsPage::WindField;
			drawWindField();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_trees"), "Trees"))) {
			ImGui::Checkbox(T(TKEY("enable_trunk_bend"), "Enable Trunk Bend"), &settings.enableTrunkBend);
			ImGui::SeparatorText(T(TKEY("trunk_wind_response"), "Tree Response"));
			ImGui::SliderFloat(T(TKEY("trunk_wind_flexible_height"), "Flexible Height"), &settings.trunkWindFlexibleHeight,
				kTrunkWindFlexibleHeightMin, kTrunkWindFlexibleHeightMax, "%.0f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_maximum_displacement"), "Maximum Displacement"), &settings.trunkWindMaximumDisplacement,
				kTrunkWindMaximumDisplacementMin, kTrunkWindMaximumDisplacementMax, "%.0f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_bend_sensitivity"), "Trunk Wind Sensitivity"), &settings.trunkWindBendSensitivity,
				kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("tree_leaf_ambient_sensitivity"), "Leaf Flutter Sensitivity"), &settings.treeLeafAmbientSensitivity,
				kTreeLeafAmbientSensitivityMin, kTreeLeafAmbientSensitivityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("tree_leaf_ambient_sensitivity_tooltip"), "Controls how strongly mean wind speed and ambient gust pressure increase Skyrim's existing leaf animation. Zero preserves vanilla motion."));
			}
			ImGui::EndTabItem();
		}

		DrawTreeMeshSettings();

		if (ImGui::BeginTabItem(T(TKEY("tab_grass"), "Grass"))) {
			ImGui::Checkbox(T(TKEY("enable_ambient_grass_wind"), "Enable Ambient Grass Wind"), &settings.enableAmbientGrassWind);
			ImGui::Checkbox(T(TKEY("override_trunk_wind_intensity"), "Override Vanilla Wind Intensity"), &settings.overrideTrunkWindIntensity);
			ImGui::BeginDisabled(!settings.overrideTrunkWindIntensity);
			ImGui::SliderFloat(T(TKEY("trunk_wind_intensity"), "Vanilla Wind Intensity"), &settings.trunkWindIntensityOverride,
				kTrunkWindIntensityMin, kTrunkWindIntensityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("trunk_wind_intensity_tooltip"), "Scales Skyrim's vanilla grass motion; ambient tree bending always uses the shared wind field."));
			}
			if (ImGui::Button(T(TKEY("reset_grass_wind_settings"), "Reset Grass Settings")))
				ResetGrassWindSettings(settings);
			ImGui::BeginDisabled(!settings.enableAmbientGrassWind);
			ImGui::SliderFloat(T(TKEY("grass_wind_response"), "Bend Strength"), &settings.grassWindResponse,
				kGrassWindResponseMin, kGrassWindResponseMax, "%.0f deg/unit", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_response_tooltip"), "Controls how strongly sampled ambient wind velocity bends the grass."));
			ImGui::SliderFloat(T(TKEY("grass_wind_maximum_tilt"), "Maximum Bend Angle"), &settings.grassWindMaximumTilt,
				kGrassWindMaximumTiltMin, kGrassWindMaximumTiltMax, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_maximum_tilt_tooltip"), "Limits how far a grass blade can lean from upright."));
			ImGui::SliderFloat(T(TKEY("grass_wind_bend_profile"), "Tip Flexibility"), &settings.grassWindBendProfile,
				kGrassWindBendProfileMin, kGrassWindBendProfileMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_bend_profile_tooltip"), "Zero leans the whole blade uniformly; one concentrates bending toward the tip."));
			ImGui::SeparatorText(T(TKEY("grass_wind_spring"), "Spring / Recovery"));
			ImGui::Checkbox(T(TKEY("grass_wind_bend_target_spring"), "Spring Bend Target"), &settings.grassWindUseBendTargetSpring);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_bend_target_spring_tooltip"), "Enabled applies spring response to the desired bend angle; disabled applies it to sampled wind velocity."));
			ImGui::SliderFloat(T(TKEY("grass_wind_spring_lag"), "Response Lag"), &settings.grassWindSpringLag,
				kGrassWindSpringLagMin, kGrassWindSpringLagMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_spring_lag_tooltip"), "Delays the grass response behind the traveling ambient gust field."));
			ImGui::SliderFloat(T(TKEY("grass_wind_spring_strength"), "Inertia"), &settings.grassWindSpringStrength,
				kGrassWindSpringStrengthMin, kGrassWindSpringStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_spring_strength_tooltip"), "Controls how strongly grass follows the delayed gust response."));
			ImGui::SliderFloat(T(TKEY("grass_wind_spring_recovery"), "Recovery Overshoot"), &settings.grassWindSpringRecovery,
				kGrassWindSpringRecoveryMin, kGrassWindSpringRecoveryMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_spring_recovery_tooltip"), "Adds a small opposite bend as grass recovers after a gust passes."));
			ImGui::SeparatorText(T(TKEY("grass_wind_flutter"), "Flutter"));
			ImGui::SliderFloat(T(TKEY("grass_wind_flutter_strength"), "Flutter Strength"), &settings.grassWindFlutterStrength,
				kGrassWindFlutterStrengthMin, kGrassWindFlutterStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_flutter_strength_tooltip"), "Scales Skyrim's vanilla grass motion before the ambient bend is applied; one matches vanilla intensity."));
			ImGui::SliderFloat(T(TKEY("grass_wind_flutter_frequency"), "Maximum Flutter Frequency"), &settings.grassWindFlutterFrequency,
				kGrassWindFlutterFrequencyMin, kGrassWindFlutterFrequencyMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_flutter_frequency_tooltip"), "Caps automatic flutter frequency: it rises from 1x at zero wind to this value at maximum wind."));
			ImGui::EndDisabled();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_atmosphere"), "Atmosphere"))) {
			activeSettingsPage = SettingsPage::Atmosphere;
			ImGui::SliderFloat(T(TKEY("sky_brightness"), "Sky Brightness"), &settings.skyBrightness, kSkyBrightnessMin, kSkyBrightnessMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndTabItem();
		}

		DrawWaterSettings();

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

void CSUtility::SetTreeWindTestEnabled(bool a_enabled)
{
	if (treeWindTest.enabled == a_enabled)
		return;

	if (a_enabled) {
		const auto* state = globals::state;
		treeWindTest.speed = ClampFiniteOrDefault(state ? state->windFieldSelectedSpeed : windFieldOverrideSpeed, 0.0f, 2.0f, 1.0f);
		treeWindTest.gustScale = ClampFiniteOrDefault(state ? state->windFieldTuning.gustScale : settings.windFieldGustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, settings.windFieldGustScale);
		treeWindTest.gustAmplitude = ClampFiniteOrDefault(state ? state->windFieldTuning.gustAmplitude : settings.windFieldGustAmplitude,
			kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, settings.windFieldGustAmplitude);
		treeWindTest.gustAdvectionMultiplier = ClampFiniteOrDefault(
			state ? state->windFieldTuning.gustAdvectionMultiplier : settings.windFieldGustAdvectionMultiplier,
			kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, settings.windFieldGustAdvectionMultiplier);
	}
	treeWindTest.enabled = a_enabled;
}

void CSUtility::DrawTreeWindTestSettings()
{
	if (!treeWindTest.enabled) {
		if (const auto* state = globals::state)
			treeWindTest.speed = ClampFiniteOrDefault(state->windFieldSelectedSpeed, 0.0f, 2.0f, 1.0f);
		treeWindTest.gustScale = settings.windFieldGustScale;
		treeWindTest.gustAmplitude = settings.windFieldGustAmplitude;
		treeWindTest.gustAdvectionMultiplier = settings.windFieldGustAdvectionMultiplier;
	}

	if (!ImGui::TreeNodeEx(T(TKEY("tree_wind_test_conditions"), "Test Wind Conditions"), ImGuiTreeNodeFlags_DefaultOpen))
		return;

	bool testEnabled = treeWindTest.enabled;
	if (ImGui::Checkbox(T(TKEY("tree_wind_test_enable"), "Override Wind for Testing"), &testEnabled))
		SetTreeWindTestEnabled(testEnabled);
	ImGui::TextWrapped("%s", T(TKEY("tree_wind_test_runtime_note"),
								 "Runtime only. Disable this override to immediately return control to weather and the Wind Field settings."));

	ImGui::BeginDisabled(!treeWindTest.enabled);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_speed"), "Wind Speed"), &treeWindTest.speed, 0.0f, 2.0f, "%.3f",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_advection"), "Gust Advection Multiplier"),
		&treeWindTest.gustAdvectionMultiplier, kWindFieldGustAdvectionMultiplierMin,
		kWindFieldGustAdvectionMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_scale"), "Gust Spatial Scale"), &treeWindTest.gustScale,
		kWindFieldGustScaleMin, kWindFieldGustScaleMax, "%.0f units",
		ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_amplitude"), "Gust Amplitude"), &treeWindTest.gustAmplitude,
		kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::TreePop();
}

void CSUtility::DrawTreeMeshSettings()
{
	if (!ImGui::BeginTabItem(T(TKEY("tab_tree_meshes"), "Tree Meshes")))
		return;

	activeSettingsPage = SettingsPage::TreeMeshes;
	DrawTreeWindTestSettings();
	const std::size_t ruleCount = TreeWindPatcher::GetRuleCount();
	const std::size_t unsavedCount = TreeWindPatcher::GetUnsavedRuleCount();
	ImGui::TextWrapped("%s", T(TKEY("tree_mesh_live_note"),
								 "Changes apply immediately to every loaded instance of the selected mesh. Save rewrites NatureOfTheWildLands.json."));
	ImGui::Text("%s: %zu    %s: %zu", T(TKEY("tree_mesh_rule_count"), "Meshes"), ruleCount,
		T(TKEY("tree_mesh_unsaved_count"), "Unsaved"), unsavedCount);

	ImGui::SetNextItemWidth(-1.0f);
	const bool searchChanged = ImGui::InputTextWithHint("##TreeMeshSearch",
		T(TKEY("tree_mesh_search_hint"), "Search mesh paths..."), treeMeshSearch.data(), treeMeshSearch.size());
	const std::string normalizedSearch = Util::FixFilePath(std::string(treeMeshSearch.data()));
	if (searchChanged || filteredTreeRuleCount != ruleCount || appliedTreeMeshSearch != normalizedSearch) {
		filteredTreeRuleIndices.clear();
		filteredTreeRuleIndices.reserve(ruleCount);
		for (std::size_t index = 0; index < ruleCount; ++index) {
			const auto rule = TreeWindPatcher::GetRule(index);
			if (normalizedSearch.empty() || rule.mesh.find(normalizedSearch) != std::string_view::npos)
				filteredTreeRuleIndices.push_back(index);
		}
		filteredTreeRuleCount = ruleCount;
		appliedTreeMeshSearch = normalizedSearch;
	}

	ImGui::BeginDisabled(unsavedCount == 0);
	if (ImGui::Button(T(TKEY("tree_mesh_save"), "Save JSON"))) {
		const auto result = TreeWindPatcher::SaveRules();
		treeWindSaveSucceeded = result.success;
		treeWindSaveStatus = result.success ?
		                         std::format("Saved {} meshes to {}", result.savedRuleCount, result.path) :
		                         std::format("Save failed: {}", result.error);
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("tree_mesh_revert"), "Revert Unsaved"))) {
		TreeWindPatcher::RevertUnsavedChanges();
		treeWindSaveStatus.clear();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("tree_mesh_restore_backup"), "Restore Backup")))
		ImGui::OpenPopup("##RestoreTreeWindBackup");

	if (ImGui::BeginPopupModal("##RestoreTreeWindBackup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", T(TKEY("tree_mesh_restore_backup_warning"),
									 "Replace the editable tree wind JSON and all live values with the startup backup?"));
		if (ImGui::Button(T(TKEY("tree_mesh_restore_confirm"), "Restore"))) {
			const auto result = TreeWindPatcher::RestoreBackup();
			treeWindSaveSucceeded = result.success;
			treeWindSaveStatus = result.success ?
			                         std::format("Restored {} meshes from backup", result.savedRuleCount) :
			                         std::format("Restore failed: {}", result.error);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("cancel"), "Cancel")))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (!treeWindSaveStatus.empty()) {
		const ImVec4 statusColor = treeWindSaveSucceeded ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
		ImGui::TextColored(statusColor, "%s", treeWindSaveStatus.c_str());
	}

	ImGui::Text("%s: %zu", T(TKEY("tree_mesh_search_results"), "Matches"), filteredTreeRuleIndices.size());
	const auto& style = ImGui::GetStyle();
	const float tableBottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - style.WindowPadding.y - style.ItemSpacing.y;
	const float visibleTableHeight = tableBottom - ImGui::GetCursorScreenPos().y;
	const float minimumTableHeight = ImGui::GetTextLineHeightWithSpacing() * 5.0f;
	const float tableHeight = std::max(visibleTableHeight, minimumTableHeight);
	const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
	                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##TreeMeshRules", 3, tableFlags, ImVec2(0.0f, tableHeight))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_path"), "Mesh"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_bend"), "Bend"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_leaf"), "Leaf Flutter"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(filteredTreeRuleIndices.size()));
		while (clipper.Step()) {
			for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
				const auto ruleIndex = filteredTreeRuleIndices[static_cast<std::size_t>(visibleIndex)];
				const auto rule = TreeWindPatcher::GetRule(ruleIndex);
				float bend = rule.bend;
				float leafAmbient = rule.leafAmbient;
				bool changed = false;

				ImGui::PushID(static_cast<int>(rule.id));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const std::string compactPath = CompactMeshPath(rule.mesh, ImGui::GetContentRegionAvail().x);
				ImGui::TextUnformatted(compactPath.c_str());
				if (compactPath != rule.mesh && ImGui::IsItemHovered())
					ImGui::SetTooltip("%.*s", static_cast<int>(rule.mesh.size()), rule.mesh.data());
				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##Bend", &bend, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(2);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##Leaf", &leafAmbient, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				if (changed) {
					(void)TreeWindPatcher::SetRule(ruleIndex, bend, leafAmbient);
					treeWindSaveStatus.clear();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}

	ImGui::EndTabItem();
}

json CSUtility::GetDiagnostics()
{
	return json{
		{ "treeWindRuleCount", TreeWindPatcher::GetRuleCount() },
		{ "treeWindUnsavedRuleCount", TreeWindPatcher::GetUnsavedRuleCount() },
		{ "treeWindTestEnabled", treeWindTest.enabled },
		{ "treeWindTestSpeed", treeWindTest.speed },
		{ "treeWindTestGustScale", treeWindTest.gustScale },
		{ "treeWindTestGustAmplitude", treeWindTest.gustAmplitude },
		{ "treeWindTestGustAdvectionMultiplier", treeWindTest.gustAdvectionMultiplier },
	};
}

void CSUtility::RegisterUxActions()
{
	FEATURE_COMMAND("setTreeWindRule",
		"Apply live per-model tree wind tuning. Params: mesh (string), bendSensitivity (number, 0-4), leafAmbientSensitivity (number, 0-4).",
		[](Feature*, const json& args) {
			if (!args.contains("mesh") || !args["mesh"].is_string() ||
				!args.contains("bendSensitivity") || !args["bendSensitivity"].is_number() ||
				!args.contains("leafAmbientSensitivity") || !args["leafAmbientSensitivity"].is_number()) {
				logger::warn("[TreeWindPatcher] Devbench setTreeWindRule received invalid arguments");
				return;
			}
			if (!TreeWindPatcher::SetRule(args["mesh"].get<std::string>(), args["bendSensitivity"].get<float>(),
					args["leafAmbientSensitivity"].get<float>())) {
				logger::warn("[TreeWindPatcher] Devbench setTreeWindRule did not match mesh {}", args["mesh"].get<std::string>());
			}
		});
	FEATURE_COMMAND("saveTreeWindRules", "Write all current live tree wind values to NatureOfTheWildLands.json.",
		[](Feature*, const json&) {
			const auto result = TreeWindPatcher::SaveRules();
			if (!result.success)
				logger::error("[TreeWindPatcher] Devbench save failed: {}", result.error);
		});
	FEATURE_COMMAND("revertTreeWindRules", "Revert live tree wind edits made since the JSON was loaded or saved.",
		[](Feature*, const json&) { TreeWindPatcher::RevertUnsavedChanges(); });
	FEATURE_COMMAND("restoreTreeWindBackup", "Replace NatureOfTheWildLands.json and live values with its startup backup.",
		[](Feature*, const json&) {
			const auto result = TreeWindPatcher::RestoreBackup();
			if (!result.success)
				logger::error("[TreeWindPatcher] Devbench backup restore failed: {}", result.error);
		});
	FEATURE_COMMAND("setTreeWindTestConditions",
		"Set runtime-only tree wind test conditions. Optional params: enabled (boolean), speed (0-2), gustScale (128-16384), gustAmplitude (0-1), gustAdvectionMultiplier (0-8).",
		[](Feature* feature, const json& args) {
			auto* utility = static_cast<CSUtility*>(feature);
			if (args.contains("enabled") && args["enabled"].is_boolean())
				utility->SetTreeWindTestEnabled(args["enabled"].get<bool>());
			if (args.contains("speed") && args["speed"].is_number())
				utility->treeWindTest.speed = ClampFiniteOrDefault(args["speed"].get<float>(), 0.0f, 2.0f, utility->treeWindTest.speed);
			if (args.contains("gustScale") && args["gustScale"].is_number())
				utility->treeWindTest.gustScale = ClampFiniteOrDefault(args["gustScale"].get<float>(),
					kWindFieldGustScaleMin, kWindFieldGustScaleMax, utility->treeWindTest.gustScale);
			if (args.contains("gustAmplitude") && args["gustAmplitude"].is_number())
				utility->treeWindTest.gustAmplitude = ClampFiniteOrDefault(args["gustAmplitude"].get<float>(),
					kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, utility->treeWindTest.gustAmplitude);
			if (args.contains("gustAdvectionMultiplier") && args["gustAdvectionMultiplier"].is_number())
				utility->treeWindTest.gustAdvectionMultiplier = ClampFiniteOrDefault(args["gustAdvectionMultiplier"].get<float>(),
					kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax,
					utility->treeWindTest.gustAdvectionMultiplier);
		});
	FEATURE_QUERY("treeWindRules",
		"Search live tree wind rules. Params: search (string, optional), offset (integer, default 0), limit (integer, 1-500, default 100).",
		([](const Feature*, const json& args) -> json {
			const std::string search = Util::FixFilePath(args.value("search", std::string{}));
			const std::size_t offset = static_cast<std::size_t>(std::max(args.value("offset", 0), 0));
			const std::size_t limit = static_cast<std::size_t>(std::clamp(args.value("limit", 100), 1, 500));
			json matches = json::array();
			std::size_t matchIndex = 0;
			std::size_t totalMatches = 0;
			for (std::size_t index = 0; index < TreeWindPatcher::GetRuleCount(); ++index) {
				const auto rule = TreeWindPatcher::GetRule(index);
				if (!search.empty() && rule.mesh.find(search) == std::string_view::npos)
					continue;
				if (matchIndex++ >= offset && matches.size() < limit) {
					matches.push_back({
						{ "mesh", rule.mesh },
						{ "bendSensitivity", rule.bend },
						{ "leafAmbientSensitivity", rule.leafAmbient },
						{ "unsaved", rule.unsaved },
					});
				}
				++totalMatches;
			}
			return json{
				{ "totalRules", TreeWindPatcher::GetRuleCount() },
				{ "totalMatches", totalMatches },
				{ "unsavedRules", TreeWindPatcher::GetUnsavedRuleCount() },
				{ "rules", std::move(matches) },
			};
		}));
}

json CSUtility::GetRuntimeFlags()
{
	return json{
		{ "VisualizeWindField", visualizeWindField },
		{ "WindFieldUseRealSpeed", windFieldUseRealSpeed },
		{ "WindFieldUseRealDirection", windFieldUseRealDirection },
		{ "TreeWindTestOverride", treeWindTest.enabled },
	};
}

bool CSUtility::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "VisualizeWindField") {
		visualizeWindField = a_value;
		return true;
	}
	if (a_name == "WindFieldUseRealSpeed") {
		windFieldUseRealSpeed = a_value;
		return true;
	}
	if (a_name == "WindFieldUseRealDirection") {
		windFieldUseRealDirection = a_value;
		return true;
	}
	if (a_name == "TreeWindTestOverride") {
		SetTreeWindTestEnabled(a_value);
		return true;
	}
	return false;
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
	case SettingsPage::WindField:
		settings.windFieldGustScale = defaults.windFieldGustScale;
		settings.windFieldGustAmplitude = defaults.windFieldGustAmplitude;
		settings.windFieldGustAdvectionMultiplier = defaults.windFieldGustAdvectionMultiplier;
		break;
	case SettingsPage::TreeMeshes:
		TreeWindPatcher::RevertUnsavedChanges();
		break;
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
	}
}

bool CSUtility::ReapplyCurrentPageOverrideSettings()
{
	static constexpr std::array<std::string_view, 1> atmosphereKeys{ "skyBrightness" };
	static constexpr std::array<std::string_view, 3> windFieldKeys{
		"windFieldGustScale",
		"windFieldGustAmplitude",
		"windFieldGustAdvectionMultiplier"
	};
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

	switch (activeSettingsPage) {
	case SettingsPage::WindField:
		return ReapplyOverrideSettingsForKeys(windFieldKeys);
	case SettingsPage::TreeMeshes:
		TreeWindPatcher::RevertUnsavedChanges();
		return true;
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
	TreeWindPatcher::LoadAndInstall();
	Hooks::Install();
	InstallDepthOfFieldHooks();
}

void CSUtility::DataLoaded()
{
	UnderwaterDepthOfField::InstallHooks();
}

#undef I18N_KEY_PREFIX
