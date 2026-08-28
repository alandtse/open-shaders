#include "CSUtility.h"

#include "Bloom.h"
#include "Globals.h"
#include "GpuPass.h"
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
#include "Utils/TransientWindImpulse.h"
#include "Utils/UI.h"
#include "Utils/WindEffects/FusRoDahWind.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <random>
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
	constexpr float kTreeWindUpperBendRangeMin = 5.0f;
	constexpr float kTreeWindUpperBendRangeMax = 100.0f;
	constexpr float kTreeWindMaximumDisplacementPercentMin = 0.0f;
	constexpr float kTreeWindMaximumDisplacementPercentMax = 10.0f;
	constexpr float kTrunkWindSensitivityMin = 0.0f;
	constexpr float kTrunkWindSensitivityMax = 20.0f;
	constexpr float kTreeWindSpringStrengthMin = 0.05f;
	constexpr float kTreeWindSpringStrengthMax = 4.0f;
	constexpr float kTreeWindSpringDampingMin = 0.0f;
	constexpr float kTreeWindSpringDampingMax = 1.0f;
	constexpr float kTreeTransientWindInfluenceMin = 0.0f;
	constexpr float kTreeTransientWindInfluenceMax = 5.0f;
	constexpr float kTreeTransientMaximumBendMultiplierMin = 0.0f;
	constexpr float kTreeTransientMaximumBendMultiplierMax = 5.0f;
	constexpr float kTreeWindGustInfluenceMin = 0.0f;
	constexpr float kTreeWindGustInfluenceMax = 2.0f;
	constexpr float kTreeLeafBaseWindFlutterGainMin = 0.0f;
	constexpr float kTreeLeafBaseWindFlutterGainMax = 8.0f;
	constexpr float kWindFieldGustScaleMin = 128.0f;
	constexpr float kWindFieldGustScaleMax = 16384.0f;
	constexpr float kWindFieldGustAmplitudeMin = 0.0f;
	constexpr float kWindFieldGustAmplitudeMax = 1.0f;
	constexpr float kWindFieldGustAdvectionMultiplierMin = 0.0f;
	constexpr float kWindFieldGustAdvectionMultiplierMax = 8.0f;
	constexpr float kWindFieldDirectionTransitionDurationMin = 0.0f;
	constexpr float kWindFieldDirectionTransitionDurationMax = 30.0f;
	constexpr float kFusRoDahIntensityMin = 0.0f;
	constexpr float kFusRoDahIntensityMax = 5.0f;
	constexpr float kFusRoDahDecayTimeMin = 0.0f;
	constexpr float kFusRoDahDecayTimeMax = WindField::kTransientImpulseMaximumDecayTime;
	constexpr float kFusRoDahDistanceMultiplierMin = 0.25f;
	constexpr float kFusRoDahDistanceMultiplierMax = 3.0f;
	constexpr float kFusRoDahWidthMultiplierMin = 0.25f;
	constexpr float kFusRoDahWidthMultiplierMax = 3.0f;
	constexpr float kFusRoDahSpeedMultiplierMin = 0.25f;
	constexpr float kFusRoDahSpeedMultiplierMax = 3.0f;
	constexpr float kFusRoDahConeHalfAngleMin = 5.0f;
	constexpr float kFusRoDahConeHalfAngleMax = 90.0f;
	constexpr float kGrassWindResponseMin = 0.0f;
	constexpr float kGrassWindResponseMax = 180.0f;
	constexpr float kGrassWindSensitivityMin = 0.0f;
	constexpr float kGrassWindSensitivityMax = 5.0f;
	constexpr float kGrassWindMaximumTiltMin = 0.0f;
	constexpr float kGrassWindMaximumTiltMax = 89.0f;
	constexpr float kGrassWindBendProfileMin = 0.0f;
	constexpr float kGrassWindBendProfileMax = 1.0f;
	constexpr float kGrassWindSpringFrequencyMin = 0.25f;
	constexpr float kGrassWindSpringFrequencyMax = 8.0f;
	constexpr float kGrassWindSpringDampingMin = 0.5f;
	constexpr float kGrassWindSpringDampingMax = 1.5f;
	constexpr uint32_t kGrassWindSpringQualityRangeCount = 3;
	constexpr std::array<std::string_view, kGrassWindSpringQualityRangeCount> kGrassWindSpringQualityRangeNames{ "Near", "Mid", "Far" };
	constexpr std::array<uint32_t, 6> kGrassWindSpringTextureSizes{ 32, 64, 128, 256, 512, 1024 };
	constexpr uint32_t kGrassWindSpringTextureCount = 4;
	constexpr uint32_t kGrassWindSpringBytesPerPixel = 8;
	constexpr double kBytesPerMiB = 1024.0 * 1024.0;
	constexpr float kGrassWindSpringDistanceMin = 1000.0f;
	constexpr float kGrassWindSpringDistanceMax = 32768.0f;
	constexpr float kGrassWindSpringLagMin = 0.0f;
	constexpr float kGrassWindSpringLagMax = 0.5f;
	constexpr float kGrassWindSpringRecoveryLagMin = 0.0f;
	constexpr float kGrassWindSpringRecoveryLagMax = 5.0f;
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

	uint32_t SanitizeGrassWindSpringTextureSize(uint32_t a_textureSize)
	{
		return *std::min_element(kGrassWindSpringTextureSizes.begin(), kGrassWindSpringTextureSizes.end(),
			[a_textureSize](uint32_t a_left, uint32_t a_right) {
				return std::abs(static_cast<int64_t>(a_left) - a_textureSize) <
			           std::abs(static_cast<int64_t>(a_right) - a_textureSize);
			});
	}

	void SanitizeSettings(CSUtility::Settings& a_settings)
	{
		const CSUtility::Settings defaults{};
		a_settings.trunkWindIntensityOverride = ClampFiniteOrDefault(a_settings.trunkWindIntensityOverride, kTrunkWindIntensityMin, kTrunkWindIntensityMax, defaults.trunkWindIntensityOverride);
		a_settings.trunkWindBendSensitivity = ClampFiniteOrDefault(a_settings.trunkWindBendSensitivity, kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, defaults.trunkWindBendSensitivity);
		a_settings.treeWindSpringStrength = ClampFiniteOrDefault(a_settings.treeWindSpringStrength, kTreeWindSpringStrengthMin, kTreeWindSpringStrengthMax, defaults.treeWindSpringStrength);
		a_settings.treeWindSpringDamping = ClampFiniteOrDefault(a_settings.treeWindSpringDamping, kTreeWindSpringDampingMin, kTreeWindSpringDampingMax, defaults.treeWindSpringDamping);
		a_settings.treeTransientWindInfluence = ClampFiniteOrDefault(a_settings.treeTransientWindInfluence, kTreeTransientWindInfluenceMin, kTreeTransientWindInfluenceMax, defaults.treeTransientWindInfluence);
		a_settings.treeTransientMaximumBendMultiplier = ClampFiniteOrDefault(a_settings.treeTransientMaximumBendMultiplier, kTreeTransientMaximumBendMultiplierMin, kTreeTransientMaximumBendMultiplierMax, defaults.treeTransientMaximumBendMultiplier);
		a_settings.treeLeafBaseWindFlutterGain = ClampFiniteOrDefault(a_settings.treeLeafBaseWindFlutterGain, kTreeLeafBaseWindFlutterGainMin, kTreeLeafBaseWindFlutterGainMax, defaults.treeLeafBaseWindFlutterGain);
		a_settings.windFieldGustScale = ClampFiniteOrDefault(a_settings.windFieldGustScale, kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
		a_settings.windFieldGustAmplitude = ClampFiniteOrDefault(a_settings.windFieldGustAmplitude, kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, defaults.windFieldGustAmplitude);
		a_settings.windFieldGustAdvectionMultiplier = ClampFiniteOrDefault(a_settings.windFieldGustAdvectionMultiplier, kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, defaults.windFieldGustAdvectionMultiplier);
		a_settings.windFieldDirectionTransitionDuration = ClampFiniteOrDefault(a_settings.windFieldDirectionTransitionDuration, kWindFieldDirectionTransitionDurationMin, kWindFieldDirectionTransitionDurationMax, defaults.windFieldDirectionTransitionDuration);
		a_settings.fusRoDahIntensity = ClampFiniteOrDefault(a_settings.fusRoDahIntensity, kFusRoDahIntensityMin, kFusRoDahIntensityMax, defaults.fusRoDahIntensity);
		a_settings.fusRoDahDecayTime = ClampFiniteOrDefault(a_settings.fusRoDahDecayTime, kFusRoDahDecayTimeMin, kFusRoDahDecayTimeMax, defaults.fusRoDahDecayTime);
		a_settings.fusRoDahDistanceMultiplier = ClampFiniteOrDefault(a_settings.fusRoDahDistanceMultiplier, kFusRoDahDistanceMultiplierMin, kFusRoDahDistanceMultiplierMax, defaults.fusRoDahDistanceMultiplier);
		a_settings.fusRoDahWidthMultiplier = ClampFiniteOrDefault(a_settings.fusRoDahWidthMultiplier, kFusRoDahWidthMultiplierMin, kFusRoDahWidthMultiplierMax, defaults.fusRoDahWidthMultiplier);
		a_settings.fusRoDahSpeedMultiplier = ClampFiniteOrDefault(a_settings.fusRoDahSpeedMultiplier, kFusRoDahSpeedMultiplierMin, kFusRoDahSpeedMultiplierMax, defaults.fusRoDahSpeedMultiplier);
		a_settings.fusRoDahConeHalfAngle = ClampFiniteOrDefault(a_settings.fusRoDahConeHalfAngle, kFusRoDahConeHalfAngleMin, kFusRoDahConeHalfAngleMax, defaults.fusRoDahConeHalfAngle);
		a_settings.grassWindResponse = ClampFiniteOrDefault(a_settings.grassWindResponse, kGrassWindResponseMin, kGrassWindResponseMax, defaults.grassWindResponse);
		a_settings.grassWindSensitivity = ClampFiniteOrDefault(a_settings.grassWindSensitivity, kGrassWindSensitivityMin, kGrassWindSensitivityMax, defaults.grassWindSensitivity);
		a_settings.grassWindMaximumTilt = ClampFiniteOrDefault(a_settings.grassWindMaximumTilt, kGrassWindMaximumTiltMin, kGrassWindMaximumTiltMax, defaults.grassWindMaximumTilt);
		a_settings.grassWindBendProfile = ClampFiniteOrDefault(a_settings.grassWindBendProfile, kGrassWindBendProfileMin, kGrassWindBendProfileMax, defaults.grassWindBendProfile);
		a_settings.grassWindSpringFrequency = ClampFiniteOrDefault(a_settings.grassWindSpringFrequency, kGrassWindSpringFrequencyMin, kGrassWindSpringFrequencyMax, defaults.grassWindSpringFrequency);
		a_settings.grassWindSpringDamping = ClampFiniteOrDefault(a_settings.grassWindSpringDamping, kGrassWindSpringDampingMin, kGrassWindSpringDampingMax, defaults.grassWindSpringDamping);
		for (uint32_t index = 0; index < kGrassWindSpringQualityRangeCount; ++index) {
			a_settings.grassWindSpringQuality[index].textureSize =
				SanitizeGrassWindSpringTextureSize(a_settings.grassWindSpringQuality[index].textureSize);
			a_settings.grassWindSpringQuality[index].maxDistance = ClampFiniteOrDefault(
				a_settings.grassWindSpringQuality[index].maxDistance,
				kGrassWindSpringDistanceMin,
				kGrassWindSpringDistanceMax,
				defaults.grassWindSpringQuality[index].maxDistance);
			if (index > 0)
				a_settings.grassWindSpringQuality[index].maxDistance =
					std::max(a_settings.grassWindSpringQuality[index].maxDistance,
						a_settings.grassWindSpringQuality[index - 1].maxDistance);
		}
		a_settings.grassWindSpringLag = ClampFiniteOrDefault(a_settings.grassWindSpringLag, kGrassWindSpringLagMin, kGrassWindSpringLagMax, defaults.grassWindSpringLag);
		a_settings.grassWindSpringRecoveryLag = ClampFiniteOrDefault(a_settings.grassWindSpringRecoveryLag, kGrassWindSpringRecoveryLagMin, kGrassWindSpringRecoveryLagMax, defaults.grassWindSpringRecoveryLag);
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
		a_settings.grassWindUseSpringField = defaults.grassWindUseSpringField;
		a_settings.grassWindResponse = defaults.grassWindResponse;
		a_settings.grassWindSensitivity = defaults.grassWindSensitivity;
		a_settings.grassWindMaximumTilt = defaults.grassWindMaximumTilt;
		a_settings.grassWindBendProfile = defaults.grassWindBendProfile;
		a_settings.grassWindSpringFrequency = defaults.grassWindSpringFrequency;
		a_settings.grassWindSpringDamping = defaults.grassWindSpringDamping;
		a_settings.grassWindSpringQuality = defaults.grassWindSpringQuality;
		a_settings.grassWindSpringLag = defaults.grassWindSpringLag;
		a_settings.grassWindSpringRecoveryLag = defaults.grassWindSpringRecoveryLag;
		a_settings.grassWindSpringStrength = defaults.grassWindSpringStrength;
		a_settings.grassWindSpringRecovery = defaults.grassWindSpringRecovery;
		a_settings.grassWindUseBendTargetSpring = defaults.grassWindUseBendTargetSpring;
		a_settings.grassWindFlutterStrength = defaults.grassWindFlutterStrength;
		a_settings.grassWindFlutterFrequency = defaults.grassWindFlutterFrequency;
		a_settings.grassWindUseVanillaFlutter = defaults.grassWindUseVanillaFlutter;
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
	CSUtility::Settings::GrassWindSpringQualityRange,
	textureSize,
	maxDistance)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CSUtility::Settings,
	enableTrunkBend,
	overrideTrunkWindIntensity,
	trunkWindIntensityOverride,
	trunkWindBendSensitivity,
	treeWindSpringStrength,
	treeWindSpringDamping,
	treeTransientWindInfluence,
	treeTransientMaximumBendMultiplier,
	treeLeafBaseWindFlutterGain,
	windFieldGustScale,
	windFieldGustAmplitude,
	windFieldGustAdvectionMultiplier,
	windFieldDirectionTransitionDuration,
	enableFusRoDahWind,
	fusRoDahIntensity,
	fusRoDahDecayTime,
	fusRoDahDistanceMultiplier,
	fusRoDahWidthMultiplier,
	fusRoDahSpeedMultiplier,
	fusRoDahConeHalfAngle,
	enableAmbientGrassWind,
	grassWindUseSpringField,
	grassWindResponse,
	grassWindSensitivity,
	grassWindMaximumTilt,
	grassWindBendProfile,
	grassWindSpringFrequency,
	grassWindSpringDamping,
	grassWindSpringQuality,
	grassWindSpringLag,
	grassWindSpringRecoveryLag,
	grassWindSpringStrength,
	grassWindSpringRecovery,
	grassWindUseBendTargetSpring,
	grassWindFlutterStrength,
	grassWindFlutterFrequency,
	grassWindUseVanillaFlutter,
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

void CSUtility::DrawWindSettings()
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
		if (ImGui::SliderFloat(T(TKEY("wind_field_override_speed"), "Wind Debug: Override Speed"), &windFieldOverrideSpeed,
				0.0f, 2.0f, "%.3f"))
			windFieldUseRealSpeed = false;
		if (treeWindTest.enabled) {
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
				T(TKEY("tree_wind_test_active_notice"), "Tree Meshes runtime test conditions currently override wind speed and gust tuning."));
		}
		if (ImGui::Checkbox(T(TKEY("wind_field_use_real_direction"), "Wind Debug: Use Real Wind Direction"),
				&windFieldUseRealDirection) &&
			!windFieldUseRealDirection) {
			const auto* state = globals::state;
			const float currentDirectionDegrees = state ?
			                                          std::atan2(state->windFieldCurrent.direction.y, state->windFieldCurrent.direction.x) *
			                                              (180.0f / 3.14159265358979323846f) :
			                                          0.0f;
			windFieldPendingDirectionDegrees = currentDirectionDegrees;
			windFieldAppliedDirectionDegrees = currentDirectionDegrees;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("wind_field_use_real_direction_tooltip"),
				"Use the current weather wind direction. Disable this to stage and apply a manual field direction."));
		}
		ImGui::SliderFloat(T(TKEY("wind_field_target_direction"), "Target Direction"),
			&windFieldPendingDirectionDegrees, -180.0f, 180.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::Button(T(TKEY("wind_field_apply_direction"), "Apply Direction"))) {
			windFieldAppliedDirectionDegrees = windFieldPendingDirectionDegrees;
			windFieldUseRealDirection = false;
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("wind_field_random_direction"), "New Random Wind Direction"))) {
			static std::mt19937 randomGenerator{ std::random_device{}() };
			static std::uniform_real_distribution<float> randomDirectionDegrees(-180.0f, 180.0f);
			windFieldPendingDirectionDegrees = randomDirectionDegrees(randomGenerator);
			windFieldAppliedDirectionDegrees = windFieldPendingDirectionDegrees;
			windFieldUseRealDirection = false;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_field_random_direction_tooltip"),
				"Choose and apply a new random fixed direction immediately."));
		ImGui::SliderFloat(T(TKEY("wind_field_transition_duration"), "Direction Blend Period"),
			&settings.windFieldDirectionTransitionDuration, kWindFieldDirectionTransitionDurationMin,
			kWindFieldDirectionTransitionDurationMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_field_transition_duration_tooltip"),
				"A direction change creates a new, fixed-orientation noise field and lerps the old field into it over this period."));
		static constexpr const char* debugViewLabels[]{ "Blended Output", "Current Field", "Previous Field", "Both Fields", "Comparison", "Spring Field", "Transient Impulses" };
		int debugView = static_cast<int>(windFieldDebugView);
		if (ImGui::Combo(T(TKEY("wind_field_debug_view"), "Wind Debug View"), &debugView, debugViewLabels,
				static_cast<int>(std::size(debugViewLabels)))) {
			windFieldDebugView = static_cast<WindFieldDebugView>(debugView);
			visualizeWindField = true;
		}
		ImGui::SeparatorText(T(TKEY("wind_field_live_values"), "Wind Field Live Values"));
		auto* const state = globals::state;
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
		ImGui::Text("Current field: direction (%.3f, %.3f), travel %.1f", state->windFieldCurrent.direction.x,
			state->windFieldCurrent.direction.y, state->windFieldCurrent.travelDistance);
		if (state->windFieldTransitionActive) {
			ImGui::Text("Previous field: direction (%.3f, %.3f), travel %.1f", state->windFieldTransition.direction.x,
				state->windFieldTransition.direction.y, state->windFieldTransition.travelDistance);
			ImGui::Text("Transition: %.0f%%", state->windFieldTransitionBlend * 100.0f);
		}
		if (sky)
			ImGui::Text("%s: speed %.5f, angle %.5f", T(TKEY("wind_field_sky_input"), "Sky wind input"), sky->windSpeed, sky->windAngle);
		if (weather)
			ImGui::Text("%s: %.3f (raw %u), direction raw %u", T(TKEY("wind_field_weather_input"), "Weather input"),
				static_cast<unsigned>(weather->data.windSpeed) / 255.0f, static_cast<unsigned>(weather->data.windSpeed),
				static_cast<unsigned>(weather->data.windDirection));
		const float selectedSpeed = state->windFieldSelectedSpeed;
		const float appliedDirectionRadians = DirectX::XMConvertToRadians(windFieldAppliedDirectionDegrees);
		const float selectedDirectionX = windFieldUseRealDirection && ambientDirectionLength > 0.0001f ?
		                                     state->ambientWindVelocity.x / ambientDirectionLength :
		                                     std::cos(appliedDirectionRadians);
		const float selectedDirectionY = windFieldUseRealDirection && ambientDirectionLength > 0.0001f ?
		                                     state->ambientWindVelocity.y / ambientDirectionLength :
		                                     std::sin(appliedDirectionRadians);
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

	if (ImGui::BeginTabBar("##WindTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_wind_field"), "Wind Field"))) {
			activeSettingsPage = SettingsPage::WindField;
			drawWindField();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_fus_ro_dah"), "Fus Ro Dah"))) {
			activeSettingsPage = SettingsPage::FusRoDah;
			if (ImGui::Checkbox(T(TKEY("enable_fus_ro_dah_wind"), "Enable Wind Impulse"),
					&settings.enableFusRoDahWind) &&
				!settings.enableFusRoDahWind)
				globals::state->ClearTransientWindImpulses();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("enable_fus_ro_dah_wind_tooltip"),
					"Adds Unrelenting Force as a directional wave traveling through the shared wind field."));
			ImGui::BeginDisabled(!settings.enableFusRoDahWind);
			ImGui::SliderFloat(T(TKEY("fus_ro_dah_intensity"), "Intensity"), &settings.fusRoDahIntensity,
				kFusRoDahIntensityMin, kFusRoDahIntensityMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fus_ro_dah_intensity_tooltip"),
					"Scales the rank-specific wind velocity added at the moving pressure wave."));
			ImGui::SliderFloat(T(TKEY("fus_ro_dah_decay_time"), "Decay Time"),
				&settings.fusRoDahDecayTime, kFusRoDahDecayTimeMin,
				kFusRoDahDecayTimeMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fus_ro_dah_decay_time_tooltip"),
					"Leaves a weakening pressure wake behind the wavefront so windfield consumers settle gradually after it passes."));
			ImGui::SliderFloat(T(TKEY("fus_ro_dah_distance_multiplier"), "Propagation Distance"),
				&settings.fusRoDahDistanceMultiplier, kFusRoDahDistanceMultiplierMin,
				kFusRoDahDistanceMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fus_ro_dah_distance_multiplier_tooltip"),
					"Scales the rank-specific distance the pressure wave can travel."));
			ImGui::SliderFloat(T(TKEY("fus_ro_dah_width_multiplier"), "Wave Width"),
				&settings.fusRoDahWidthMultiplier, kFusRoDahWidthMultiplierMin,
				kFusRoDahWidthMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fus_ro_dah_width_multiplier_tooltip"),
					"Scales the thickness of the moving pressure front and its trailing wake."));
			ImGui::SliderFloat(T(TKEY("fus_ro_dah_speed_multiplier"), "Propagation Speed"),
				&settings.fusRoDahSpeedMultiplier, kFusRoDahSpeedMultiplierMin,
				kFusRoDahSpeedMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fus_ro_dah_speed_multiplier_tooltip"),
					"Scales how quickly the wavefront travels through the shared wind field."));
			ImGui::SliderFloat(T(TKEY("fus_ro_dah_cone_half_angle"), "Cone Half-Angle"),
				&settings.fusRoDahConeHalfAngle, kFusRoDahConeHalfAngleMin,
				kFusRoDahConeHalfAngleMax, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fus_ro_dah_cone_half_angle_tooltip"),
					"Controls the forward cone width. Smaller angles make the impulse more directional."));
			ImGui::EndDisabled();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_trees"), "Trees"))) {
			activeSettingsPage = SettingsPage::Trees;
			ImGui::Checkbox(T(TKEY("enable_trunk_bend"), "Enable Trunk Bend"), &settings.enableTrunkBend);
			ImGui::SeparatorText(T(TKEY("trunk_wind_response"), "Tree Response"));
			ImGui::SliderFloat(T(TKEY("trunk_wind_bend_sensitivity"), "Trunk Wind Sensitivity"), &settings.trunkWindBendSensitivity,
				kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("tree_transient_wind_influence"), "Transient Wind Influence"), &settings.treeTransientWindInfluence,
				kTreeTransientWindInfluenceMin, kTreeTransientWindInfluenceMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("tree_transient_wind_influence_tooltip"),
					"Scales transient wind impulses applied to tree trunks and leaves without changing ambient wind or grass."));
			ImGui::SliderFloat(T(TKEY("tree_transient_maximum_bend_multiplier"), "Transient Maximum Bend"),
				&settings.treeTransientMaximumBendMultiplier, kTreeTransientMaximumBendMultiplierMin,
				kTreeTransientMaximumBendMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("tree_transient_maximum_bend_multiplier_tooltip"),
					"Sets the transient bend limit relative to the normal tree maximum displacement."));
			ImGui::SliderFloat(T(TKEY("tree_wind_spring_strength"), "Spring Strength"), &settings.treeWindSpringStrength,
				kTreeWindSpringStrengthMin, kTreeWindSpringStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("tree_wind_spring_strength_tooltip"), "Controls how quickly the spring pulls trees toward the raw ambient wind target. Lower values feel heavier and lag longer."));
			ImGui::SliderFloat(T(TKEY("tree_wind_spring_damping"), "Spring Damping"), &settings.treeWindSpringDamping,
				kTreeWindSpringDampingMin, kTreeWindSpringDampingMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("tree_wind_spring_damping_tooltip"), "Reduces overshoot while preserving gradual recovery after the wind weakens."));
			ImGui::SeparatorText(T(TKEY("tree_leaf_flutter"), "Leaf Flutter"));
			ImGui::SliderFloat(T(TKEY("tree_leaf_base_wind_flutter_gain"), "Base Wind Flutter Gain"),
				&settings.treeLeafBaseWindFlutterGain, kTreeLeafBaseWindFlutterGainMin,
				kTreeLeafBaseWindFlutterGainMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("tree_leaf_base_wind_flutter_gain_tooltip"),
					"Controls the direct leaf animation strength driven by shared base wind. Gust influence adds only local amplitude variation."));
			}
			ImGui::EndTabItem();
		}

		DrawTreeMeshSettings();

		if (ImGui::BeginTabItem(T(TKEY("tab_grass"), "Grass"))) {
			ImGui::Checkbox(T(TKEY("enable_ambient_grass_wind"), "Enable Ambient Grass Wind"), &settings.enableAmbientGrassWind);
			ImGui::Checkbox(T(TKEY("grass_wind_use_spring_field"), "A/B: Use Compute Spring Field"), &settings.grassWindUseSpringField);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_use_spring_field_tooltip"),
					"Enabled uses the cached compute-shader spring field; disabled uses the previous per-vertex spring path."));
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
			ImGui::SliderFloat(T(TKEY("grass_wind_sensitivity"), "Wind Sensitivity"), &settings.grassWindSensitivity,
				kGrassWindSensitivityMin, kGrassWindSensitivityMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_sensitivity_tooltip"), "Scales grass-only wind speed. At 2x, wind at speed 1 is treated as speed 2 for grass bending and flutter."));
			ImGui::SliderFloat(T(TKEY("grass_wind_maximum_tilt"), "Maximum Bend Angle"), &settings.grassWindMaximumTilt,
				kGrassWindMaximumTiltMin, kGrassWindMaximumTiltMax, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_maximum_tilt_tooltip"), "Limits how far a grass blade can lean from upright."));
			ImGui::SliderFloat(T(TKEY("grass_wind_bend_profile"), "Tip Flexibility"), &settings.grassWindBendProfile,
				kGrassWindBendProfileMin, kGrassWindBendProfileMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_bend_profile_tooltip"), "Zero leans the whole blade uniformly; one concentrates bending toward the tip."));
			ImGui::SeparatorText(T(TKEY("grass_wind_spring"), "Physical Spring"));
			ImGui::SliderFloat(T(TKEY("grass_wind_spring_frequency"), "Natural Frequency"),
				&settings.grassWindSpringFrequency, kGrassWindSpringFrequencyMin,
				kGrassWindSpringFrequencyMax, "%.2f Hz", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_spring_frequency_tooltip"),
					"Controls how quickly blades react and rebound. Lower values feel heavier; higher values feel stiffer."));
			ImGui::SliderFloat(T(TKEY("grass_wind_spring_damping"), "Damping Ratio"),
				&settings.grassWindSpringDamping, kGrassWindSpringDampingMin,
				kGrassWindSpringDampingMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_spring_damping_tooltip"),
					"Below one allows a natural rebound; one is critically damped; above one settles without overshoot."));
			static constexpr const char* qualityNames[] = { "Near", "Mid", "Far" };
			static constexpr const char* textureSizeLabels[] = { "32", "64", "128", "256", "512", "1024" };
			static constexpr const char* textureSizeKeys[] = {
				"grass_wind_spring_near_texture_size",
				"grass_wind_spring_mid_texture_size",
				"grass_wind_spring_far_texture_size"
			};
			static constexpr const char* textureSizeDefaults[] = {
				"Near Field Texture Size",
				"Mid Field Texture Size",
				"Far Field Texture Size"
			};
			static constexpr const char* distanceKeys[] = {
				"grass_wind_spring_near_distance",
				"grass_wind_spring_mid_distance",
				"grass_wind_spring_far_distance"
			};
			static constexpr const char* distanceDefaults[] = {
				"Near Range End",
				"Mid Range End",
				"Far Range End"
			};
			for (uint32_t index = 0; index < kGrassWindSpringQualityRangeCount; ++index) {
				ImGui::SeparatorText(qualityNames[index]);
				int textureSizeIndex = 0;
				for (std::size_t option = 0; option < kGrassWindSpringTextureSizes.size(); ++option) {
					if (settings.grassWindSpringQuality[index].textureSize == kGrassWindSpringTextureSizes[option]) {
						textureSizeIndex = static_cast<int>(option);
						break;
					}
				}
				if (ImGui::Combo(T(textureSizeKeys[index], textureSizeDefaults[index]), &textureSizeIndex,
						textureSizeLabels, static_cast<int>(std::size(textureSizeLabels))))
					settings.grassWindSpringQuality[index].textureSize = kGrassWindSpringTextureSizes[textureSizeIndex];
				ImGui::SliderFloat(T(distanceKeys[index], distanceDefaults[index]),
					&settings.grassWindSpringQuality[index].maxDistance, kGrassWindSpringDistanceMin,
					kGrassWindSpringDistanceMax, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
			}
			for (uint32_t index = 1; index < kGrassWindSpringQualityRangeCount; ++index)
				settings.grassWindSpringQuality[index].maxDistance =
					std::max(settings.grassWindSpringQuality[index].maxDistance,
						settings.grassWindSpringQuality[index - 1].maxDistance);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_spring_quality_tooltip"),
					"Each field covers a radial quality range. The next range begins where the previous range ends; grass beyond the far range uses the fallback path."));
			ImGui::SeparatorText(T(TKEY("grass_wind_flutter"), "Flutter"));
			ImGui::Checkbox(T(TKEY("grass_wind_use_vanilla_flutter"), "A/B: Use Vanilla-Style Flutter"),
				&settings.grassWindUseVanillaFlutter);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_use_vanilla_flutter_tooltip"),
					"Enabled adds Skyrim-style per-blade displacement to the compute spring result; disabled uses inexpensive bend modulation."));
			ImGui::SliderFloat(T(TKEY("grass_wind_flutter_strength"), "Flutter Strength"), &settings.grassWindFlutterStrength,
				kGrassWindFlutterStrengthMin, kGrassWindFlutterStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_flutter_strength_tooltip"), "Scales the selected per-blade flutter style."));
			ImGui::SliderFloat(T(TKEY("grass_wind_flutter_frequency"), "Flutter Frequency"), &settings.grassWindFlutterFrequency,
				kGrassWindFlutterFrequencyMin, kGrassWindFlutterFrequencyMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_flutter_frequency_tooltip"), "Controls how quickly the ambient bend oscillates independently for each grass blade."));
			std::array<double, kGrassWindSpringQualityRangeCount> springMemoryMiB{};
			double totalSpringMemoryMiB = 0.0;
			for (uint32_t index = 0; index < kGrassWindSpringQualityRangeCount; ++index) {
				const uint32_t textureSize = SanitizeGrassWindSpringTextureSize(
					settings.grassWindSpringQuality[index].textureSize);
				springMemoryMiB[index] = static_cast<double>(textureSize) * textureSize *
				                         kGrassWindSpringTextureCount * kGrassWindSpringBytesPerPixel / kBytesPerMiB;
				totalSpringMemoryMiB += springMemoryMiB[index];
			}
			ImGui::SeparatorText(T(TKEY("grass_wind_spring_memory"), "Spring Memory"));
			ImGui::Text("%s: %.2f MiB", T(TKEY("grass_wind_spring_memory_total"), "Estimated GPU memory reserved"), totalSpringMemoryMiB);
			ImGui::Text("%s: %.2f MiB | %s: %.2f MiB | %s: %.2f MiB",
				qualityNames[0], springMemoryMiB[0], qualityNames[1], springMemoryMiB[1], qualityNames[2], springMemoryMiB[2]);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("grass_wind_spring_memory_tooltip"),
					"Estimate for four RGBA16F textures per field: two response textures and two velocity textures."));
			ImGui::EndDisabled();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void CSUtility::DrawSettings()
{
	if (ImGui::BeginTabBar("##CSUtilityTabs", ImGuiTabBarFlags_None)) {
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
	if (ImGui::BeginTable("##TreeMeshRules", 7, tableFlags, ImVec2(0.0f, tableHeight))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_path"), "Mesh"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_bend"), "Bend"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_leaf"), "Leaf Flutter"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_upper_bend_range"), "Upper Bend"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_maximum_displacement"), "Top Displacement"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_trunk_gust"), "Trunk Gust"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_leaf_gust"), "Leaf Gust"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(filteredTreeRuleIndices.size()));
		while (clipper.Step()) {
			for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
				const auto ruleIndex = filteredTreeRuleIndices[static_cast<std::size_t>(visibleIndex)];
				const auto rule = TreeWindPatcher::GetRule(ruleIndex);
				float bend = rule.bend;
				float leafAmbient = rule.leafAmbient;
				float upperBendRange = rule.upperBendRange;
				float maximumDisplacementPercent = rule.maximumDisplacementPercent;
				float trunkGustInfluence = rule.trunkGustInfluence;
				float leafGustInfluence = rule.leafGustInfluence;
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
				ImGui::TableSetColumnIndex(3);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##UpperBend", &upperBendRange, kTreeWindUpperBendRangeMin,
					kTreeWindUpperBendRangeMax, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(4);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##MaximumDisplacement", &maximumDisplacementPercent,
					kTreeWindMaximumDisplacementPercentMin, kTreeWindMaximumDisplacementPercentMax, "%.2f%%",
					ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(5);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##TrunkGust", &trunkGustInfluence, kTreeWindGustInfluenceMin,
					kTreeWindGustInfluenceMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(6);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##LeafGust", &leafGustInfluence, kTreeWindGustInfluenceMin,
					kTreeWindGustInfluenceMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				if (changed) {
					(void)TreeWindPatcher::SetRule(ruleIndex, bend, leafAmbient, upperBendRange,
						maximumDisplacementPercent, trunkGustInfluence, leafGustInfluence);
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
		{ "treeWindSpringStrength", settings.treeWindSpringStrength },
		{ "treeWindSpringDamping", settings.treeWindSpringDamping },
		{ "treeTransientWindInfluence", settings.treeTransientWindInfluence },
		{ "treeTransientMaximumBendMultiplier", settings.treeTransientMaximumBendMultiplier },
		{ "treeLeafBaseWindFlutterGain", settings.treeLeafBaseWindFlutterGain },
	};
}

void CSUtility::RegisterUxActions()
{
	FEATURE_COMMAND("setTreeResponseDynamics",
		"Tune shared heavy tree response. Optional params: springStrength (0.05-4), damping (0-1).",
		[](Feature* feature, const json& args) {
			auto* utility = static_cast<CSUtility*>(feature);
			if (args.contains("springStrength") && args["springStrength"].is_number()) {
				utility->settings.treeWindSpringStrength = ClampFiniteOrDefault(args["springStrength"].get<float>(),
					kTreeWindSpringStrengthMin, kTreeWindSpringStrengthMax, utility->settings.treeWindSpringStrength);
			}
			if (args.contains("damping") && args["damping"].is_number()) {
				utility->settings.treeWindSpringDamping = ClampFiniteOrDefault(args["damping"].get<float>(),
					kTreeWindSpringDampingMin, kTreeWindSpringDampingMax, utility->settings.treeWindSpringDamping);
			}
		});
	FEATURE_COMMAND("setTreeWindRule",
		"Apply live per-model tree wind tuning. Params: mesh (string), bendSensitivity and leafAmbientSensitivity (0-4), "
		"upperBendRange (5-100), maximumDisplacementPercent (0-10), trunkGustInfluence and leafGustInfluence (0-2).",
		[](Feature*, const json& args) {
			if (!args.contains("mesh") || !args["mesh"].is_string() ||
				!args.contains("bendSensitivity") || !args["bendSensitivity"].is_number() ||
				!args.contains("leafAmbientSensitivity") || !args["leafAmbientSensitivity"].is_number() ||
				!args.contains("upperBendRange") || !args["upperBendRange"].is_number() ||
				!args.contains("maximumDisplacementPercent") || !args["maximumDisplacementPercent"].is_number() ||
				!args.contains("trunkGustInfluence") || !args["trunkGustInfluence"].is_number() ||
				!args.contains("leafGustInfluence") || !args["leafGustInfluence"].is_number()) {
				logger::warn("[TreeWindPatcher] Devbench setTreeWindRule received invalid arguments");
				return;
			}
			if (!TreeWindPatcher::SetRule(args["mesh"].get<std::string>(), args["bendSensitivity"].get<float>(),
					args["leafAmbientSensitivity"].get<float>(), args["upperBendRange"].get<float>(),
					args["maximumDisplacementPercent"].get<float>(), args["trunkGustInfluence"].get<float>(),
					args["leafGustInfluence"].get<float>())) {
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
	const Settings defaults{};
	settings = o_json;
	if (!o_json.contains("grassWindSpringQuality")) {
		if (o_json.contains("grassWindSpringTextureSize") && o_json["grassWindSpringTextureSize"].is_number_unsigned()) {
			const auto legacyTextureSize = o_json["grassWindSpringTextureSize"].get<uint32_t>();
			for (auto& range : settings.grassWindSpringQuality)
				range.textureSize = legacyTextureSize;
		}
		if (o_json.contains("grassWindSpringWorldSize") && o_json["grassWindSpringWorldSize"].is_number()) {
			const float legacyWorldSize = o_json["grassWindSpringWorldSize"].get<float>();
			if (std::isfinite(legacyWorldSize))
				settings.grassWindSpringQuality.back().maxDistance = legacyWorldSize * 0.5f;
		}
	}
	if (!o_json.contains("grassWindSpringFrequency") && o_json.contains("grassWindSpringLag") &&
		o_json["grassWindSpringLag"].is_number()) {
		const float legacyLag = std::max(o_json["grassWindSpringLag"].get<float>(), 0.01f);
		settings.grassWindSpringFrequency = std::clamp(
			0.25f / legacyLag, kGrassWindSpringFrequencyMin, kGrassWindSpringFrequencyMax);
	}
	if (!o_json.contains("grassWindSpringDamping"))
		settings.grassWindSpringDamping = defaults.grassWindSpringDamping;
	if (!o_json.contains("treeLeafBaseWindFlutterGain")) {
		if (o_json.contains("treeLeafWindSensitivity") && o_json["treeLeafWindSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafWindSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
		else if (o_json.contains("treeLeafAmbientSensitivity") && o_json["treeLeafAmbientSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafAmbientSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
	}
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
		settings.windFieldDirectionTransitionDuration = defaults.windFieldDirectionTransitionDuration;
		break;
	case SettingsPage::FusRoDah:
		settings.enableFusRoDahWind = defaults.enableFusRoDahWind;
		settings.fusRoDahIntensity = defaults.fusRoDahIntensity;
		settings.fusRoDahDecayTime = defaults.fusRoDahDecayTime;
		settings.fusRoDahDistanceMultiplier = defaults.fusRoDahDistanceMultiplier;
		settings.fusRoDahWidthMultiplier = defaults.fusRoDahWidthMultiplier;
		settings.fusRoDahSpeedMultiplier = defaults.fusRoDahSpeedMultiplier;
		settings.fusRoDahConeHalfAngle = defaults.fusRoDahConeHalfAngle;
		break;
	case SettingsPage::Trees:
		settings.enableTrunkBend = defaults.enableTrunkBend;
		settings.trunkWindBendSensitivity = defaults.trunkWindBendSensitivity;
		settings.treeWindSpringStrength = defaults.treeWindSpringStrength;
		settings.treeWindSpringDamping = defaults.treeWindSpringDamping;
		settings.treeTransientWindInfluence = defaults.treeTransientWindInfluence;
		settings.treeTransientMaximumBendMultiplier = defaults.treeTransientMaximumBendMultiplier;
		settings.treeLeafBaseWindFlutterGain = defaults.treeLeafBaseWindFlutterGain;
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
	static constexpr std::array<std::string_view, 4> windFieldKeys{
		"windFieldGustScale",
		"windFieldGustAmplitude",
		"windFieldGustAdvectionMultiplier",
		"windFieldDirectionTransitionDuration"
	};
	static constexpr std::array<std::string_view, 7> fusRoDahKeys{
		"enableFusRoDahWind",
		"fusRoDahIntensity",
		"fusRoDahDecayTime",
		"fusRoDahDistanceMultiplier",
		"fusRoDahWidthMultiplier",
		"fusRoDahSpeedMultiplier",
		"fusRoDahConeHalfAngle"
	};
	static constexpr std::array<std::string_view, 7> treeKeys{
		"enableTrunkBend",
		"trunkWindBendSensitivity",
		"treeWindSpringStrength",
		"treeWindSpringDamping",
		"treeTransientWindInfluence",
		"treeTransientMaximumBendMultiplier",
		"treeLeafBaseWindFlutterGain"
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
	case SettingsPage::FusRoDah:
		return ReapplyOverrideSettingsForKeys(fusRoDahKeys);
	case SettingsPage::Trees:
		return ReapplyOverrideSettingsForKeys(treeKeys);
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

void CSUtility::RecreateGrassWindSpringTextures(uint32_t a_qualityIndex, uint32_t a_textureSize)
{
	if (a_qualityIndex >= kGrassWindSpringQualityRangeCount)
		return;

	ID3D11ShaderResourceView* nullSrvs[2]{};
	globals::d3d::context->VSSetShaderResources(105 + a_qualityIndex, 1, nullSrvs);
	globals::d3d::context->VSSetShaderResources(108 + a_qualityIndex, 1, nullSrvs);
	globals::d3d::context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
	ID3D11UnorderedAccessView* nullUavs[2]{};
	globals::d3d::context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
	globals::d3d::context->CSSetShader(nullptr, nullptr, 0);

	for (uint32_t textureIndex = 0; textureIndex < 2; ++textureIndex) {
		delete grassWindSpringResponseTextures[a_qualityIndex][textureIndex];
		grassWindSpringResponseTextures[a_qualityIndex][textureIndex] = nullptr;
		delete grassWindSpringVelocityTextures[a_qualityIndex][textureIndex];
		grassWindSpringVelocityTextures[a_qualityIndex][textureIndex] = nullptr;
	}

	D3D11_TEXTURE2D_DESC textureDesc{
		.Width = a_textureSize,
		.Height = a_textureSize,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
		.Format = textureDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{
		.Format = textureDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};
	const float clearValue[4]{};
	for (uint32_t textureIndex = 0; textureIndex < 2; ++textureIndex) {
		const std::string responseName = std::format(
			"OSUtility::GrassWindSpring{}Response{}", kGrassWindSpringQualityRangeNames[a_qualityIndex], textureIndex);
		grassWindSpringResponseTextures[a_qualityIndex][textureIndex] = new Texture2D(textureDesc, responseName.c_str());
		grassWindSpringResponseTextures[a_qualityIndex][textureIndex]->CreateSRV(srvDesc);
		grassWindSpringResponseTextures[a_qualityIndex][textureIndex]->CreateUAV(uavDesc);
		globals::d3d::context->ClearUnorderedAccessViewFloat(
			grassWindSpringResponseTextures[a_qualityIndex][textureIndex]->uav.get(), clearValue);

		const std::string velocityName = std::format(
			"OSUtility::GrassWindSpring{}Velocity{}", kGrassWindSpringQualityRangeNames[a_qualityIndex], textureIndex);
		grassWindSpringVelocityTextures[a_qualityIndex][textureIndex] = new Texture2D(textureDesc, velocityName.c_str());
		grassWindSpringVelocityTextures[a_qualityIndex][textureIndex]->CreateSRV(srvDesc);
		grassWindSpringVelocityTextures[a_qualityIndex][textureIndex]->CreateUAV(uavDesc);
		globals::d3d::context->ClearUnorderedAccessViewFloat(
			grassWindSpringVelocityTextures[a_qualityIndex][textureIndex]->uav.get(), clearValue);
	}
	grassWindSpringTextureSizes[a_qualityIndex] = a_textureSize;
	grassWindSpringTextureIndices[a_qualityIndex] = 0;
	grassWindSpringInitialized[a_qualityIndex] = false;
	grassWindSpringFieldAvailable[a_qualityIndex] = false;
}

void CSUtility::SetupResources()
{
	vanillaPointLightCB = new ConstantBuffer(ConstantBufferDesc<VanillaPointLightData>(), "OSUtility::VanillaPointLightData");
	grassWindSpringCB = new ConstantBuffer(
		ConstantBufferDesc<GrassWindSpringData>(), "OSUtility::GrassWindSpringData");
	Settings sanitizedSettings = settings;
	SanitizeSettings(sanitizedSettings);
	for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex)
		RecreateGrassWindSpringTextures(qualityIndex, sanitizedSettings.grassWindSpringQuality[qualityIndex].textureSize);

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, grassWindSpringSampler.put()));
	Util::SetResourceName(grassWindSpringSampler.get(), "OSUtility::GrassWindSpringSampler");
}

void CSUtility::ClearShaderCache()
{
	grassWindSpringCS.Reset();
}

void CSUtility::UpdateGrassWindSpring()
{
	if (!grassWindSpringCB)
		return;
	for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
		if (!grassWindSpringResponseTextures[qualityIndex][0] || !grassWindSpringResponseTextures[qualityIndex][1] ||
			!grassWindSpringVelocityTextures[qualityIndex][0] || !grassWindSpringVelocityTextures[qualityIndex][1])
			return;
	}

	static Util::FrameChecker frameChecker;
	auto* context = globals::d3d::context;
	if (frameChecker.IsNewFrame()) {
		Settings sanitizedSettings = settings;
		SanitizeSettings(sanitizedSettings);
		RE::NiPoint3 center{};
		if (globals::game::player)
			center = globals::game::player->GetPosition();
		const float anchorCellSize = sanitizedSettings.grassWindSpringQuality[0].maxDistance * 2.0f /
		                             static_cast<float>(sanitizedSettings.grassWindSpringQuality[0].textureSize);
		const float2 snappedCenter{
			std::floor(center.x / anchorCellSize) * anchorCellSize,
			std::floor(center.y / anchorCellSize) * anchorCellSize
		};

		GrassWindSpringData data{};
		const float fieldHeight = center.z;
		const float frameTime = std::clamp(globals::state->windFieldFrameTime, 0.0f, 0.25f);
		const float responseRadians = DirectX::XMConvertToRadians(sanitizedSettings.grassWindResponse);
		const float maximumTiltRadians = DirectX::XMConvertToRadians(sanitizedSettings.grassWindMaximumTilt);
		const float sensitivity = sanitizedSettings.grassWindSensitivity;
		const float springFrequency = sanitizedSettings.grassWindSpringFrequency;
		const float springDamping = sanitizedSettings.grassWindSpringDamping;
		ID3D11ShaderResourceView* nullSrvs[2]{};
		ID3D11UnorderedAccessView* nullUavs[2]{};
		for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
			const auto& quality = sanitizedSettings.grassWindSpringQuality[qualityIndex];
			const float fieldSize = quality.maxDistance * 2.0f;
			if (quality.textureSize != grassWindSpringTextureSizes[qualityIndex])
				RecreateGrassWindSpringTextures(qualityIndex, quality.textureSize);
			if (fieldSize != grassWindSpringWorldSizes[qualityIndex]) {
				grassWindSpringWorldSizes[qualityIndex] = fieldSize;
				grassWindSpringInitialized[qualityIndex] = false;
				grassWindSpringFieldAvailable[qualityIndex] = false;
			}
			const float fieldHalfSize = fieldSize * 0.5f;
			const float2 nextFieldMinimum{
				snappedCenter.x - fieldHalfSize,
				snappedCenter.y - fieldHalfSize
			};
			previousGrassWindSpringFieldMinimum[qualityIndex] = grassWindSpringInitialized[qualityIndex] ?
			                                                        grassWindSpringFieldMinimum[qualityIndex] :
			                                                        nextFieldMinimum;
			grassWindSpringFieldMinimum[qualityIndex] = nextFieldMinimum;
			data.fields[qualityIndex] = {
				grassWindSpringFieldMinimum[qualityIndex],
				previousGrassWindSpringFieldMinimum[qualityIndex],
				fieldHeight,
				frameTime,
				responseRadians,
				maximumTiltRadians,
				sensitivity,
				springFrequency,
				springDamping,
				grassWindSpringInitialized[qualityIndex] ? 0u : 1u,
				0u,
				fieldSize,
				quality.textureSize,
				quality.maxDistance
			};
		}

		ID3D11Buffer* constantBuffers[]{ grassWindSpringCB->CB(), globals::state->sharedDataCB->CB() };
		context->CSSetConstantBuffers(0, 1, constantBuffers);
		context->CSSetConstantBuffers(5, 1, constantBuffers + 1);
		auto* shader = sanitizedSettings.enableAmbientGrassWind && sanitizedSettings.grassWindUseSpringField ?
		                   grassWindSpringCS.Get(
							   L"Data\\Shaders\\GrassWindSpringCS.hlsl", {}, "cs_5_0", "main",
							   "OSUtility::GrassWindSpringCS") :
		                   nullptr;
		for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
			data.activeField = qualityIndex;
			data.fields[qualityIndex].fieldAvailable = 0u;
			grassWindSpringCB->Update(data);
			const uint32_t currentTextureIndex = grassWindSpringTextureIndices[qualityIndex];
			const uint32_t outputTextureIndex = currentTextureIndex ^ 1u;
			ID3D11ShaderResourceView* srvs[]{
				grassWindSpringResponseTextures[qualityIndex][currentTextureIndex]->srv.get(),
				grassWindSpringVelocityTextures[qualityIndex][currentTextureIndex]->srv.get()
			};
			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
			ID3D11UnorderedAccessView* uavs[]{
				grassWindSpringResponseTextures[qualityIndex][outputTextureIndex]->uav.get(),
				grassWindSpringVelocityTextures[qualityIndex][outputTextureIndex]->uav.get()
			};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			grassWindSpringFieldAvailable[qualityIndex] = false;
			if (shader) {
				context->CSSetShader(shader, nullptr, 0);
				CS_GPU_PASS("OSUtility::GrassWindSpringUpdate");
				context->Dispatch((data.fields[qualityIndex].textureSize + 7) / 8,
					(data.fields[qualityIndex].textureSize + 7) / 8, 1);
				grassWindSpringTextureIndices[qualityIndex] = outputTextureIndex;
				grassWindSpringFieldAvailable[qualityIndex] = true;
				if (!grassWindSpringInitialized[qualityIndex]) {
					context->CSSetShaderResources(0, ARRAYSIZE(srvs), nullSrvs);
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), nullUavs, nullptr);
					const uint32_t previousTextureIndex = grassWindSpringTextureIndices[qualityIndex] ^ 1u;
					context->CopyResource(
						grassWindSpringResponseTextures[qualityIndex][previousTextureIndex]->resource.get(),
						grassWindSpringResponseTextures[qualityIndex][grassWindSpringTextureIndices[qualityIndex]]->resource.get());
					context->CopyResource(
						grassWindSpringVelocityTextures[qualityIndex][previousTextureIndex]->resource.get(),
						grassWindSpringVelocityTextures[qualityIndex][grassWindSpringTextureIndices[qualityIndex]]->resource.get());
				}
				grassWindSpringInitialized[qualityIndex] = true;
			}
			if (!grassWindSpringFieldAvailable[qualityIndex])
				grassWindSpringInitialized[qualityIndex] = false;
		}

		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetConstantBuffers(5, 1, &nullBuffer);
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);

		data.activeField = 0u;
		for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
			data.fields[qualityIndex].initialize = 0u;
			data.fields[qualityIndex].fieldAvailable = grassWindSpringFieldAvailable[qualityIndex] ? 1u : 0u;
		}
		grassWindSpringCB->Update(data);
	}

	ID3D11Buffer* springBuffer = grassWindSpringCB->CB();
	context->VSSetConstantBuffers(9, 1, &springBuffer);
	std::array<ID3D11ShaderResourceView*, kGrassWindSpringQualityRangeCount> currentSpringSrvs{};
	std::array<ID3D11ShaderResourceView*, kGrassWindSpringQualityRangeCount> previousSpringSrvs{};
	for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
		const uint32_t currentTextureIndex = grassWindSpringTextureIndices[qualityIndex];
		currentSpringSrvs[qualityIndex] = grassWindSpringResponseTextures[qualityIndex][currentTextureIndex]->srv.get();
		previousSpringSrvs[qualityIndex] = grassWindSpringResponseTextures[qualityIndex][currentTextureIndex ^ 1u]->srv.get();
	}
	context->VSSetShaderResources(105, static_cast<UINT>(currentSpringSrvs.size()), currentSpringSrvs.data());
	context->VSSetShaderResources(108, static_cast<UINT>(previousSpringSrvs.size()), previousSpringSrvs.data());
	ID3D11SamplerState* samplers[]{ grassWindSpringSampler.get() };
	context->VSSetSamplers(14, ARRAYSIZE(samplers), samplers);
}

ID3D11ShaderResourceView* CSUtility::GetGrassWindSpringDebugSRV() const
{
	return grassWindSpringFieldAvailable[0] && grassWindSpringResponseTextures[0][grassWindSpringTextureIndices[0]] ?
	           grassWindSpringResponseTextures[0][grassWindSpringTextureIndices[0]]->srv.get() :
	           nullptr;
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
	data.windFieldDebugEnabled = visualizeWindField ? 1u : 0u;
	data.windFieldDebugView = static_cast<uint32_t>(windFieldDebugView);
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
	FusRoDahWind::Register();
}

#undef I18N_KEY_PREFIX
