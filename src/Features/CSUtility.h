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
		float trunkWindFlexibleHeight = 4096.0f;
		float trunkWindMaximumDisplacement = 512.0f;
		float trunkWindBendSensitivity = 1.0f;
		float trunkWindLeafSensitivity = 1.0f;
		float trunkWindInstanceResponseMin = 0.9f;
		float trunkWindInstanceResponseMax = 1.1f;
		float trunkWindGustStrengthMin = 0.8f;
		float trunkWindGustStrengthMax = 1.25f;
		float trunkWindGustHoldMin = 5.0f;
		float trunkWindGustHoldMax = 10.0f;
		float trunkWindGustTransitionDuration = 0.25f;
		float trunkWindVariationMin = 0.85f;
		float trunkWindVariationMax = 1.1f;
		float trunkWindVariationInterval = 2.5f;
		bool enableGrassWindExperiment = true;
		bool enableGrassWindGusts = true;
		float grassWindDisplacementScale = 1.5f;
		float grassWindMaximumDisplacement = 64.0f;
		float grassWindWaveSize = 966.0f;
		float grassWindWaveSpeed = 1.35f;
		float grassWindWaveStrength = 1.5f;
		float grassWindFlutterStrength = 0.2f;
		float grassWindFlutterSpeed = 4.5f;
		float grassWindVerticalBend = 0.18f;
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
