#pragma once

#include "Bloom.h"
#include "Buffer.h"
#include "Feature.h"
#include "I18n/I18n.h"

#include <cstdint>

struct CSUtility : Feature
{
	static CSUtility* GetSingleton()
	{
		static CSUtility singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "CS Utility"; }
	virtual std::string GetDisplayName() override { return T("feature.cs_utility.name", "OS Utility"); }
	virtual inline std::string GetShortName() override { return "CSUtility"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CS_UTILITY"; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_shaderType) override { return a_shaderType == RE::BSShader::Type::Lighting || a_shaderType == RE::BSShader::Type::Water || a_shaderType == RE::BSShader::Type::ImageSpace; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.cs_utility.description", "Core utility controls for shared renderer tuning."),
			{ T("feature.cs_utility.key_feature_1", "Atmosphere brightness control"),
				T("feature.cs_utility.key_feature_2", "Shared lighting multiplier controls"),
				T("feature.cs_utility.key_feature_3", "Separate controls for linear point lights") } };
	}

	struct DepthOfFieldAutoFocusSettings
	{
		float nearDistance = 0.0f;
		float farDistance = 0.0f;
		float nearRange = 0.0f;
		float farRange = 0.0f;
		float nearBlur = 0.0f;
		float farBlur = 0.0f;
		float blurMultiplier = 1.0f;
	};

	struct DepthOfFieldSettings
	{
		float strength = 0.0f;
		float distance = 0.0f;
		float range = 0.0f;
		uint32_t mode = 2;
		bool excludeSky = false;
		bool autoFocus = false;
		DepthOfFieldAutoFocusSettings autoFocusSettings;
		uint32_t blurRadius = 2;
	};

	struct DepthOfFieldOverride
	{
		bool locked = false;
		DepthOfFieldSettings values;
		DepthOfFieldSettings baseline;
	};

	struct Settings
	{
		bool enableTrunkBend = true;
		bool disableVanillaTreeAnimation = false;
		bool overrideTrunkWindIntensity = false;
		float trunkWindIntensityOverride = 1.0f;
		float trunkWindFlexibleHeight = 16384.0f;
		float trunkWindMaximumDisplacement = 4096.0f;
		float trunkWindBendSensitivity = 1.04f;
		float trunkWindLeafSensitivity = 7.84f;
		float trunkWindInstanceResponseMin = 1.2f;
		float trunkWindInstanceResponseMax = 1.78f;
		float trunkWindGustStrengthMin = 1.51f;
		float trunkWindGustStrengthMax = 3.0f;
		float trunkWindGustHoldMin = 7.37f;
		float trunkWindGustHoldMax = 24.57f;
		float trunkWindGustTransitionDuration = 2.23f;
		float trunkWindVariationMin = 0.85f;
		float trunkWindVariationMax = 1.14f;
		float trunkWindVariationInterval = 2.36f;
		bool enableGrassWindExperiment = true;
		bool enableGrassWindGusts = true;
		float grassWindBendScale = 1.0f;
		float grassWindMaximumBendAngle = 75.0f;
		float grassWindCurvature = 0.35f;
		float grassWindBounceStrength = 0.61f;
		float grassWindBounceFrequency = 1.1f;
		float grassWindCoarseScale = 3278.0f;
		float grassWindCoarseSpeed = 0.7f;
		float grassWindCoarseStrength = 0.35f;
		float grassWindFineScale = 380.0f;
		float grassWindFineSpeed = 2.94f;
		float grassWindFineStrength = 0.12f;
		float grassWindFlutterStrength = 0.08f;
		float grassWindFlutterSpeed = 4.33f;
		float skyBrightness = 1.0f;
		float directionalLightMult = 1.0f;
		float pointLightMult = 1.0f;
		float linearPointLightMult = 1.0f;
		float spotlightMult = 1.0f;
		float linearSpotlightMult = 1.0f;
		float omnidirectionalBulbMult = 1.0f;
		float linearOmnidirectionalBulbMult = 1.0f;
		DepthOfFieldOverride sceneDof;
		DepthOfFieldOverride underwaterDof;
		Bloom::PresetSettings bloomEnhancement;
	} settings;

	struct alignas(16) PerFrameData
	{
		float skyBrightness;
		float directionalLightMult;
		float pointLightMult;
		float linearPointLightMult;
		float spotlightMult;
		float linearSpotlightMult;
		float omnidirectionalBulbMult;
		float linearOmnidirectionalBulbMult;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 32);

	struct alignas(16) VanillaPointLightData
	{
		uint32_t pointLightFlags[8];
	};
	STATIC_ASSERT_ALIGNAS_16(VanillaPointLightData);
	static_assert(sizeof(VanillaPointLightData) == 32);

	ConstantBuffer* vanillaPointLightCB = nullptr;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;

	PerFrameData GetCommonBufferData() const;
	void UpdateVanillaPointLightData(RE::BSRenderPass* a_pass, uint32_t a_lightCount);
	void DrawDepthOfFieldSettings();
	void DrawVanillaBloomSettings();
	void InstallDepthOfFieldHooks();

	static void SanitizeDepthOfFieldSettings(DepthOfFieldSettings& a_settings);
	static void SanitizeDepthOfFieldOverride(DepthOfFieldOverride& a_override);

	struct Hooks;
};
