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
	virtual bool HasShaderDefine(RE::BSShader::Type a_shaderType) override { return a_shaderType == RE::BSShader::Type::Lighting || a_shaderType == RE::BSShader::Type::Water || a_shaderType == RE::BSShader::Type::ImageSpace || a_shaderType == RE::BSShader::Type::Effect; }
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

	struct WaterSettings
	{
		float brightness = 1.0f;
		float reflectionAmount = 1.0f;
		float refractionAmount = 1.0f;
		float sunSpecularMultiplier = 1.0f;
		float waveAmplitude = 1.0f;
		float fresnelMin = 0.0f;
		float fresnelMax = 1.0f;
		float muddiness = 1.0f;
	};

	struct FireEffectSettings
	{
		bool enabled = true;
		float displacement = 0.08f;
		float noiseScale = 0.16f;
		float speed = 1.4f;
		float opacityMin = 0.25f;
		float opacityMax = 0.70f;
		float colorMin = 0.45f;
		float colorMax = 0.82f;
		float depthFadeDistance = 12.0f;
		float fresnelPower = 1.5f;
		float edgeFade = 0.12f;
		float emission = 2.5f;
		float4 lowColor{ 1.0f, 0.12f, 0.01f, 0.0f };
		float4 highColor{ 1.0f, 0.78f, 0.22f, 0.0f };
	};

	struct Settings
	{
		float skyBrightness = 1.0f;
		float directionalLightMult = 1.0f;
		float pointLightMult = 1.0f;
		float linearPointLightMult = 1.0f;
		float spotlightMult = 1.0f;
		float linearSpotlightMult = 1.0f;
		float omnidirectionalBulbMult = 1.0f;
		float linearOmnidirectionalBulbMult = 1.0f;
		WaterSettings water;
		FireEffectSettings fireEffects;
		DepthOfFieldOverride sceneDof;
		DepthOfFieldOverride underwaterDof;
		Bloom::PresetSettings bloomEnhancement;
	} settings;

	/** Identifies the OS Utility tab targeted by scoped default restoration. */
	enum class SettingsPage
	{
		Atmosphere,           ///< Sky atmosphere controls.
		Water,                ///< Water rendering controls.
		Multipliers,          ///< Lighting multiplier controls.
		VanillaDepthOfField,  ///< Vanilla depth-of-field controls.
		VanillaBloom,         ///< Vanilla bloom controls.
		FireEffects           ///< Fire effect controls.
	};
	/** The visible tab whose settings Restore Defaults changes. */
	SettingsPage activeSettingsPage = SettingsPage::Atmosphere;

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
		float waterBrightness;
		float waterReflectionAmount;
		float waterRefractionAmount;
		float waterSunSpecularMultiplier;
		float waterWaveAmplitude;
		float waterFresnelMin;
		float waterFresnelMax;
		float waterMuddiness;
		uint enableFireEffects;
		float fireDisplacement;
		float fireNoiseScale;
		float fireSpeed;
		float fireOpacityMin;
		float fireOpacityMax;
		float fireColorMin;
		float fireColorMax;
		float fireDepthFadeDistance;
		float fireFresnelPower;
		float fireEdgeFade;
		float fireEmission;
		float4 fireLowColor;
		float4 fireHighColor;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 144);

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
	/** @return true because OS Utility supports restoring the active tab. */
	virtual bool HasScopedDefaultSettings() const override { return true; }
	/** Restores default settings for the active OS Utility tab. */
	virtual void RestoreCurrentPageDefaultSettings() override;
	/** @return true because OS Utility reapplies overrides for the active tab. */
	virtual bool HasScopedOverrideSettings() const override { return true; }
	/** Reapplies override-controlled settings for the active OS Utility tab. */
	virtual bool ReapplyCurrentPageOverrideSettings() override;
	virtual void SetupResources() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;

	PerFrameData GetCommonBufferData() const;
	void UpdateVanillaPointLightData(RE::BSRenderPass* a_pass, uint32_t a_lightCount);
	/** Marks fire Effect passes for procedural flame shading. */
	void ModifyEffect(RE::BSRenderPass* a_pass) const;
	void DrawDepthOfFieldSettings();
	/** Draws water tuning controls. */
	void DrawWaterSettings();
	void DrawVanillaBloomSettings();
	void InstallDepthOfFieldHooks();

	static void SanitizeDepthOfFieldSettings(DepthOfFieldSettings& a_settings);
	static void SanitizeDepthOfFieldOverride(DepthOfFieldOverride& a_override);
	/** Clamps water controls before serialization or GPU upload. */
	static void SanitizeWaterSettings(WaterSettings& a_settings);

	struct Hooks;
};
