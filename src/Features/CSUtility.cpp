#include "CSUtility.h"

#include "Bloom.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "LightLimitFix.h"
#include "LinearLighting.h"
#include "State.h"
#include "UnderwaterDepthOfField.h"
#include "Utils/Game.h"
#include "Utils/PointLightFlags.h"
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
	constexpr float kTrunkWindIntensityMin = 0.0f;
	constexpr float kTrunkWindIntensityMax = 10.0f;
	constexpr float kTrunkWindFlexibleHeightMin = 1.0f;
	constexpr float kTrunkWindFlexibleHeightMax = 16384.0f;
	constexpr float kTrunkWindMaximumDisplacementMin = 0.0f;
	constexpr float kTrunkWindMaximumDisplacementMax = 4096.0f;
	constexpr float kTrunkWindSensitivityMin = 0.0f;
	constexpr float kTrunkWindSensitivityMax = 20.0f;
	constexpr float kTrunkWindResponseMin = 0.0f;
	constexpr float kTrunkWindResponseMax = 3.0f;
	constexpr float kTrunkWindGustStrengthMin = 0.0f;
	constexpr float kTrunkWindGustStrengthMax = 3.0f;
	constexpr float kTrunkWindGustHoldMin = 0.25f;
	constexpr float kTrunkWindGustHoldMax = 60.0f;
	constexpr float kTrunkWindGustTransitionMin = 0.01f;
	constexpr float kTrunkWindGustTransitionMax = 5.0f;
	constexpr float kTrunkWindVariationScaleMin = 0.0f;
	constexpr float kTrunkWindVariationScaleMax = 3.0f;
	constexpr float kTrunkWindVariationIntervalMin = 0.1f;
	constexpr float kTrunkWindVariationIntervalMax = 20.0f;
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
	constexpr float kGrassWindFlutterFrequencyMin = 1.0f;
	constexpr float kGrassWindFlutterFrequencyMax = 4.0f;
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
		a_settings.trunkWindLeafSensitivity = ClampFiniteOrDefault(a_settings.trunkWindLeafSensitivity, kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, defaults.trunkWindLeafSensitivity);
		a_settings.trunkWindInstanceResponseMin = ClampFiniteOrDefault(a_settings.trunkWindInstanceResponseMin, kTrunkWindResponseMin, kTrunkWindResponseMax, defaults.trunkWindInstanceResponseMin);
		a_settings.trunkWindInstanceResponseMax = ClampFiniteOrDefault(a_settings.trunkWindInstanceResponseMax, kTrunkWindResponseMin, kTrunkWindResponseMax, defaults.trunkWindInstanceResponseMax);
		a_settings.trunkWindGustStrengthMin = ClampFiniteOrDefault(a_settings.trunkWindGustStrengthMin, kTrunkWindGustStrengthMin, kTrunkWindGustStrengthMax, defaults.trunkWindGustStrengthMin);
		a_settings.trunkWindGustStrengthMax = ClampFiniteOrDefault(a_settings.trunkWindGustStrengthMax, kTrunkWindGustStrengthMin, kTrunkWindGustStrengthMax, defaults.trunkWindGustStrengthMax);
		a_settings.trunkWindGustHoldMin = ClampFiniteOrDefault(a_settings.trunkWindGustHoldMin, kTrunkWindGustHoldMin, kTrunkWindGustHoldMax, defaults.trunkWindGustHoldMin);
		a_settings.trunkWindGustHoldMax = ClampFiniteOrDefault(a_settings.trunkWindGustHoldMax, kTrunkWindGustHoldMin, kTrunkWindGustHoldMax, defaults.trunkWindGustHoldMax);
		a_settings.trunkWindGustTransitionDuration = ClampFiniteOrDefault(a_settings.trunkWindGustTransitionDuration, kTrunkWindGustTransitionMin, kTrunkWindGustTransitionMax, defaults.trunkWindGustTransitionDuration);
		a_settings.trunkWindVariationMin = ClampFiniteOrDefault(a_settings.trunkWindVariationMin, kTrunkWindVariationScaleMin, kTrunkWindVariationScaleMax, defaults.trunkWindVariationMin);
		a_settings.trunkWindVariationMax = ClampFiniteOrDefault(a_settings.trunkWindVariationMax, kTrunkWindVariationScaleMin, kTrunkWindVariationScaleMax, defaults.trunkWindVariationMax);
		a_settings.trunkWindVariationInterval = ClampFiniteOrDefault(a_settings.trunkWindVariationInterval, kTrunkWindVariationIntervalMin, kTrunkWindVariationIntervalMax, defaults.trunkWindVariationInterval);
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
		if (a_settings.trunkWindInstanceResponseMin > a_settings.trunkWindInstanceResponseMax)
			std::swap(a_settings.trunkWindInstanceResponseMin, a_settings.trunkWindInstanceResponseMax);
		if (a_settings.trunkWindGustStrengthMin > a_settings.trunkWindGustStrengthMax)
			std::swap(a_settings.trunkWindGustStrengthMin, a_settings.trunkWindGustStrengthMax);
		if (a_settings.trunkWindGustHoldMin > a_settings.trunkWindGustHoldMax)
			std::swap(a_settings.trunkWindGustHoldMin, a_settings.trunkWindGustHoldMax);
		if (a_settings.trunkWindVariationMin > a_settings.trunkWindVariationMax)
			std::swap(a_settings.trunkWindVariationMin, a_settings.trunkWindVariationMax);
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
		a_settings.enableAmbientGrassWind = defaults.enableAmbientGrassWind;
		a_settings.grassWindResponse = defaults.grassWindResponse;
		a_settings.grassWindMaximumTilt = defaults.grassWindMaximumTilt;
		a_settings.grassWindBendProfile = defaults.grassWindBendProfile;
		a_settings.grassWindSpringLag = defaults.grassWindSpringLag;
		a_settings.grassWindSpringStrength = defaults.grassWindSpringStrength;
		a_settings.grassWindSpringRecovery = defaults.grassWindSpringRecovery;
		a_settings.grassWindFlutterStrength = defaults.grassWindFlutterStrength;
		a_settings.grassWindFlutterFrequency = defaults.grassWindFlutterFrequency;
	}

	void TriggerNewWindGust()
	{
		globals::state->windGustHoldRemaining = 0.0f;
		globals::state->windGustTransitioning = false;
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
	trunkWindLeafSensitivity,
	trunkWindInstanceResponseMin,
	trunkWindInstanceResponseMax,
	trunkWindGustStrengthMin,
	trunkWindGustStrengthMax,
	trunkWindGustHoldMin,
	trunkWindGustHoldMax,
	trunkWindGustTransitionDuration,
	trunkWindVariationMin,
	trunkWindVariationMax,
	trunkWindVariationInterval,
	windFieldGustScale,
	windFieldGustAmplitude,
	windFieldGustAdvectionMultiplier,
	enableAmbientGrassWind,
	grassWindResponse,
	grassWindMaximumTilt,
	grassWindBendProfile,
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
			ImGui::Checkbox(T(TKEY("override_trunk_wind_intensity"), "Override Wind Intensity"), &settings.overrideTrunkWindIntensity);
			ImGui::BeginDisabled(!settings.overrideTrunkWindIntensity);
			ImGui::SliderFloat(T(TKEY("trunk_wind_intensity"), "Wind Intensity"), &settings.trunkWindIntensityOverride,
				kTrunkWindIntensityMin, kTrunkWindIntensityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("trunk_wind_intensity_tooltip"), "0 is calm, 1 is full normal-scale wind, and values above 1 are exaggerated."));
			}
			ImGui::SeparatorText(T(TKEY("trunk_wind_response"), "Tree Response"));
			ImGui::SliderFloat(T(TKEY("trunk_wind_flexible_height"), "Flexible Height"), &settings.trunkWindFlexibleHeight,
				kTrunkWindFlexibleHeightMin, kTrunkWindFlexibleHeightMax, "%.0f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_maximum_displacement"), "Maximum Displacement"), &settings.trunkWindMaximumDisplacement,
				kTrunkWindMaximumDisplacementMin, kTrunkWindMaximumDisplacementMax, "%.0f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_bend_sensitivity"), "Trunk Wind Sensitivity"), &settings.trunkWindBendSensitivity,
				kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_leaf_sensitivity"), "Leaf Wind Sensitivity"), &settings.trunkWindLeafSensitivity,
				kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_response_min"), "Per-Tree Response Minimum"), &settings.trunkWindInstanceResponseMin,
				kTrunkWindResponseMin, kTrunkWindResponseMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_response_max"), "Per-Tree Response Maximum"), &settings.trunkWindInstanceResponseMax,
				kTrunkWindResponseMin, kTrunkWindResponseMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SeparatorText(T(TKEY("trunk_wind_gusts"), "Gusts"));
			ImGui::SliderFloat(T(TKEY("trunk_wind_gust_strength_min"), "Gust Strength Minimum"), &settings.trunkWindGustStrengthMin,
				kTrunkWindGustStrengthMin, kTrunkWindGustStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_gust_strength_max"), "Gust Strength Maximum"), &settings.trunkWindGustStrengthMax,
				kTrunkWindGustStrengthMin, kTrunkWindGustStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_gust_hold_min"), "Gust Hold Minimum"), &settings.trunkWindGustHoldMin,
				kTrunkWindGustHoldMin, kTrunkWindGustHoldMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_gust_hold_max"), "Gust Hold Maximum"), &settings.trunkWindGustHoldMax,
				kTrunkWindGustHoldMin, kTrunkWindGustHoldMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_gust_transition"), "Gust Transition Duration"), &settings.trunkWindGustTransitionDuration,
				kTrunkWindGustTransitionMin, kTrunkWindGustTransitionMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_variation_min"), "Gust Variation Minimum"), &settings.trunkWindVariationMin,
				kTrunkWindVariationScaleMin, kTrunkWindVariationScaleMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_variation_max"), "Gust Variation Maximum"), &settings.trunkWindVariationMax,
				kTrunkWindVariationScaleMin, kTrunkWindVariationScaleMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("trunk_wind_variation_interval"), "Gust Variation Interval"), &settings.trunkWindVariationInterval,
				kTrunkWindVariationIntervalMin, kTrunkWindVariationIntervalMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
			if (ImGui::Button(T(TKEY("trunk_wind_trigger_gust"), "Trigger New Gust")))
				TriggerNewWindGust();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_grass"), "Grass"))) {
			ImGui::Checkbox(T(TKEY("enable_ambient_grass_wind"), "Enable Ambient Grass Wind"), &settings.enableAmbientGrassWind);
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

json CSUtility::GetRuntimeFlags()
{
	return json{
		{ "VisualizeWindField", visualizeWindField },
		{ "WindFieldUseRealSpeed", windFieldUseRealSpeed },
		{ "WindFieldUseRealDirection", windFieldUseRealDirection },
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
	Hooks::Install();
	InstallDepthOfFieldHooks();
}

void CSUtility::DataLoaded()
{
	UnderwaterDepthOfField::InstallHooks();
}

#undef I18N_KEY_PREFIX
