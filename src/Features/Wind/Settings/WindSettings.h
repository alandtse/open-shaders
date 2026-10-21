#pragma once

#include <array>
#include <cstdint>
#include <string_view>

struct WindSettings
{
	struct GrassWindSpringQualityRange
	{
		uint32_t textureSize;
		float maxDistance;
	};

	bool enableTrunkBend = true;
	bool overrideTrunkWindIntensity = false;
	float trunkWindIntensityOverride = 2.22f;
	float trunkWindBendSensitivity = 1.0f;
	float treeLeafBaseWindFlutterGain = 1.2f;
	float treeWindGustScale = 1.0f;
	float treeWindGustSoftLimit = 0.75f;
	float treeWindSpringFrequency = 0.4f;
	float treeWindSpringDamping = 0.9f;
	float treeTransientSpringFrequency = 2.0f;
	float treeTransientSpringDamping = 0.7f;
	float windFieldGustScale = 600.0f;
	float windFieldGustCrosswindScale = 1100.0f;
	float windFieldGustAmplitude = 1.0f;
	float windFieldMicroTurbulenceStrength = 1.0f;
	float windFieldFineBaselineAmplitude = 0.25f;
	float windFieldFineWindCoupling = 0.35f;
	float windFieldFineGustCoupling = 1.02f;
	float windFieldFineFrequency = 1.0f;
	float windFieldGustAdvectionMultiplier = 0.99f;
	std::array<float, 3> windFieldGustAdvectionResponse{ 1.0f, 1.0f, 1.0f };
	float windFieldGustCoverage = 0.50f;
	float windFieldGustEdgeSoftness = 0.31f;
	bool enableWindFieldGustDistortion = true;
	float windFieldGustDistortionStrength = 1.0f;
	float windFieldGustDistortionScale = 1.0f;
	float windFieldGustDistortionSpeed = 0.25f;
	float windFieldDirectionTransitionDuration = 15.0f;
	bool processMidRangeTransients = true;
	bool processFarRangeTransients = false;
	float grassTransientBendStrength = 60.6f;
	float grassTransientFlutterHalfLife = 0.1f;
	float grassTransientFlutterStrength = 1.01f;
	float grassTransientFlutterFrequency = 2.99f;
	bool enableAmbientGrassWind = true;
	bool enableGrassWindSpring = true;
	bool enableGrassWindSpringBend = true;
	float grassWindResponse = 40.0f;
	float grassWindSensitivity = 1.01f;
	float grassWindMaximumTilt = 89.0f;
	float grassWindCompressionToBend = 0.5f;
	float grassWindSpringFrequency = 2.9f;
	float grassWindSpringDamping = 0.84f;
	std::array<GrassWindSpringQualityRange, 3> grassWindSpringQuality{ { { 1024, 3000.0f },
		{ 512, 6262.0f },
		{ 256, 12000.0f } } };
	float grassWindFlutterStrength = 1.0f;
	float grassWindFlutterFrequency = 1.0f;
	float grassWindFlutterGustInfluence = 2.0f;
	float grassWindFlutterWaveScale = 0.5f;
	std::array<float, 3> grassWindFlutterAmplitudeResponse{ 1.0f, 1.0f, 1.01f };
};

namespace WindSettingsLimits
{
	inline constexpr float kTrunkWindSensitivityMin = 0.0f;
	inline constexpr float kTrunkWindSensitivityMax = 20.0f;
	inline constexpr float kTreeLeafBaseWindFlutterGainMin = 0.0f;
	inline constexpr float kTreeLeafBaseWindFlutterGainMax = 20.0f;
	inline constexpr float kTreeWindGustScaleMin = 0.0f;
	inline constexpr float kTreeWindGustScaleMax = 2.0f;
	inline constexpr float kTreeWindGustSoftLimitMin = 0.0f;
	inline constexpr float kTreeWindGustSoftLimitMax = 2.0f;
	inline constexpr float kTreeWindSpringFrequencyMin = 0.25f;
	inline constexpr float kTreeWindSpringFrequencyMax = 2.0f;
	inline constexpr float kTreeWindSpringDampingMin = 0.5f;
	inline constexpr float kTreeWindSpringDampingMax = 1.5f;
	inline constexpr float kTreeTransientSpringFrequencyMin = 0.5f;
	inline constexpr float kTreeTransientSpringFrequencyMax = 8.0f;
	inline constexpr float kTreeTransientSpringDampingMin = 0.5f;
	inline constexpr float kTreeTransientSpringDampingMax = 1.5f;
	inline constexpr float kWindFieldGustScaleMin = 128.0f;
	inline constexpr float kWindFieldGustScaleMax = 16384.0f;
	inline constexpr float kWindFieldGustCrosswindScaleMin = 128.0f;
	inline constexpr float kWindFieldGustCrosswindScaleMax = 65536.0f;
	inline constexpr float kWindFieldGustAmplitudeMin = 0.0f;
	inline constexpr float kWindFieldGustAmplitudeMax = 1.0f;
	inline constexpr float kWindFieldMicroTurbulenceStrengthMin = 0.0f;
	inline constexpr float kWindFieldMicroTurbulenceStrengthMax = 4.0f;
	inline constexpr float kWindFieldFineBaselineAmplitudeMin = 0.0f;
	inline constexpr float kWindFieldFineBaselineAmplitudeMax = 0.25f;
	inline constexpr float kWindFieldFineCouplingMin = 0.0f;
	inline constexpr float kWindFieldFineCouplingMax = 4.0f;
	inline constexpr float kWindFieldFineFrequencyMin = 0.0f;
	inline constexpr float kWindFieldFineFrequencyMax = 4.0f;
	inline constexpr float kWindFieldGustAdvectionMultiplierMin = 0.0f;
	inline constexpr float kWindFieldGustAdvectionMultiplierMax = 8.0f;
	inline constexpr float kWindFieldGustCoverageMin = 0.0f;
	inline constexpr float kWindFieldGustCoverageMax = 1.0f;
	inline constexpr float kWindFieldGustEdgeSoftnessMin = 0.0f;
	inline constexpr float kWindFieldGustEdgeSoftnessMax = 1.0f;
	inline constexpr float kWindFieldGustDistortionStrengthMin = 0.0f;
	inline constexpr float kWindFieldGustDistortionStrengthMax = 2.0f;
	inline constexpr float kWindFieldGustDistortionScaleMin = 0.25f;
	inline constexpr float kWindFieldGustDistortionScaleMax = 4.0f;
	inline constexpr float kWindFieldGustDistortionSpeedMin = 0.0f;
	inline constexpr float kWindFieldGustDistortionSpeedMax = 4.0f;
	inline constexpr float kWindResponseMin = 0.0f;
	inline constexpr float kWindResponseMax = 2.0f;
	inline constexpr std::array<float, 3> kWindResponseSpeeds{ 0.1f, 0.5f, 1.0f };
	inline constexpr float kWindFieldDirectionTransitionDurationMin = 0.0f;
	inline constexpr float kWindFieldDirectionTransitionDurationMax = 30.0f;
	inline constexpr float kTrunkWindIntensityMin = 0.0f;
	inline constexpr float kTrunkWindIntensityMax = 10.0f;
	inline constexpr float kGrassWindResponseMin = 0.0f;
	inline constexpr float kGrassWindResponseMax = 180.0f;
	inline constexpr float kGrassWindSensitivityMin = 0.0f;
	inline constexpr float kGrassWindSensitivityMax = 5.0f;
	inline constexpr float kGrassWindMaximumTiltMin = 0.0f;
	inline constexpr float kGrassWindMaximumTiltMax = 89.0f;
	inline constexpr float kGrassWindCompressionToBendMin = 0.0f;
	inline constexpr float kGrassWindCompressionToBendMax = 1.0f;
	inline constexpr float kGrassWindSpringFrequencyMin = 0.25f;
	inline constexpr float kGrassWindSpringFrequencyMax = 8.0f;
	inline constexpr float kGrassWindSpringDampingMin = 0.5f;
	inline constexpr float kGrassWindSpringDampingMax = 1.5f;
	inline constexpr float kGrassWindSpringDistanceMin = 1000.0f;
	inline constexpr float kGrassWindSpringDistanceMax = 32768.0f;
	inline constexpr float kGrassWindFlutterStrengthMin = 0.0f;
	inline constexpr float kGrassWindFlutterStrengthMax = 4.0f;
	inline constexpr float kGrassWindFlutterFrequencyMin = 0.0f;
	inline constexpr float kGrassWindFlutterFrequencyMax = 4.0f;
	inline constexpr float kGrassWindFlutterGustInfluenceMin = 0.0f;
	inline constexpr float kGrassWindFlutterGustInfluenceMax = 4.0f;
	inline constexpr float kGrassWindFlutterWaveScaleMin = 0.25f;
	inline constexpr float kGrassWindFlutterWaveScaleMax = 4.0f;
	inline constexpr float kGrassWindFlutterAmplitudeResponseMin = 0.0f;
	inline constexpr float kGrassWindFlutterAmplitudeResponseMax = 4.0f;
	inline constexpr float kGrassTransientFlutterHalfLifeMin = 0.01f;
	inline constexpr float kGrassTransientFlutterHalfLifeMax = 2.0f;
	inline constexpr float kGrassTransientFlutterStrengthMin = 0.0f;
	inline constexpr float kGrassTransientFlutterStrengthMax = 2.0f;
	inline constexpr float kGrassTransientFlutterFrequencyMin = 0.25f;
	inline constexpr float kGrassTransientFlutterFrequencyMax = 12.0f;
	inline constexpr uint32_t kGrassWindSpringQualityRangeCount = 3;
	inline constexpr std::array<std::string_view, kGrassWindSpringQualityRangeCount> kGrassWindSpringQualityRangeNames{
		"Near", "Mid", "Far"
	};
	inline constexpr std::array<uint32_t, 6> kGrassWindSpringTextureSizes{ 32, 64, 128, 256, 512, 1024 };
}
