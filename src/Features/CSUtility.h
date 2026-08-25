#pragma once

#include "Bloom.h"
#include "Buffer.h"
#include "Feature.h"
#include "I18n/I18n.h"
#include "Utils/LazyShader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

	struct Settings
	{
		bool enableTrunkBend = true;
		bool overrideTrunkWindIntensity = false;
		float trunkWindIntensityOverride = 1.0f;
		float treeWindUpperBendRange = 50.0f;
		float treeWindMaximumDisplacementPercent = 3.0f;
		float trunkWindBendSensitivity = 1.04f;
		float treeWindSpringStrength = 1.0f;
		float treeWindSpringDamping = 0.7f;
		float treeWindTrunkGustInfluence = 0.1f;
		float treeLeafBaseWindFlutterGain = 3.0f;
		float treeLeafGustInfluence = 0.2f;
		float windFieldGustScale = 2048.0f;
		float windFieldGustAmplitude = 0.35f;
		float windFieldGustAdvectionMultiplier = 1.0f;
		uint32_t windFieldMaxActiveGusts = 6;
		float windFieldGustSpawnIntervalMin = 6.0f;
		float windFieldGustSpawnIntervalMax = 10.0f;
		float windFieldGustSpawnDistance = 24000.0f;
		float windFieldGustLengthMin = 16000.0f;
		float windFieldGustLengthMax = 26000.0f;
		float windFieldGustWidthMin = 3000.0f;
		float windFieldGustWidthMax = 5000.0f;
		float windFieldGustSpeedMin = 384.0f;
		float windFieldGustSpeedMax = 640.0f;
		float windFieldGustStrengthMin = 0.65f;
		float windFieldGustStrengthMax = 1.0f;
		float windFieldGustLifetimeMin = 90.0f;
		float windFieldGustLifetimeMax = 140.0f;
		float windFieldGustEdgeSoftness = 0.75f;
		float windFieldGustNoiseAmount = 0.85f;
		bool enableFusRoDahWind = true;
		float fusRoDahIntensity = 1.0f;
		float fusRoDahDecayTime = 1.5f;
		float fusRoDahDistanceMultiplier = 1.0f;
		float fusRoDahWidthMultiplier = 1.0f;
		float fusRoDahSpeedMultiplier = 1.0f;
		float fusRoDahConeHalfAngle = 41.4f;
		bool enableAmbientGrassWind = true;
		float grassWindResponse = 45.0f;
		float grassWindSensitivity = 1.0f;
		float grassWindMaximumTilt = 75.0f;
		float grassWindBendProfile = 0.35f;
		float grassWindSpringFrequency = 2.0f;
		float grassWindSpringDamping = 0.82f;
		float grassWindFlutterStrength = 1.0f;
		float grassWindFlutterFrequency = 1.0f;
		float skyBrightness = 1.0f;
		float directionalLightMult = 1.0f;
		float pointLightMult = 1.0f;
		float linearPointLightMult = 1.0f;
		float spotlightMult = 1.0f;
		float linearSpotlightMult = 1.0f;
		float omnidirectionalBulbMult = 1.0f;
		float linearOmnidirectionalBulbMult = 1.0f;
		WaterSettings water;
		DepthOfFieldOverride sceneDof;
		DepthOfFieldOverride underwaterDof;
		Bloom::PresetSettings bloomEnhancement;
	} settings;

	/** Identifies the OS Utility tab targeted by scoped default restoration. */
	enum class SettingsPage
	{
		WindField,            ///< Ambient wind-field controls.
		FusRoDah,             ///< Unrelenting Force wind impulse controls.
		Trees,                ///< Shared tree response controls.
		TreeMeshes,           ///< Live per-mesh tree wind tuning.
		Atmosphere,           ///< Sky atmosphere controls.
		Water,                ///< Water rendering controls.
		Multipliers,          ///< Lighting multiplier controls.
		VanillaDepthOfField,  ///< Vanilla depth-of-field controls.
		VanillaBloom          ///< Vanilla bloom controls.
	};
	/** The visible tab whose settings Restore Defaults changes. */
	SettingsPage activeSettingsPage = SettingsPage::Atmosphere;
	bool visualizeWindField = false;        ///< Runtime-only GPU wind-field visualization toggle.
	bool windFieldUseRealSpeed = true;      ///< Runtime-only debug input toggle.
	bool windFieldUseRealDirection = true;  ///< Runtime-only debug input toggle.
	float windFieldOverrideSpeed = 1.0f;    ///< Runtime-only speed used when real speed is disabled.
	struct RuntimeWindTest
	{
		bool enabled = false;
		float speed = 1.0f;
		float gustScale = 2048.0f;
		float gustAmplitude = 0.35f;
		float gustAdvectionMultiplier = 1.0f;
	} treeWindTest;
	/** Search and feedback state for the live tree mesh editor. */
	std::array<char, 256> treeMeshSearch{};
	std::vector<std::size_t> filteredTreeRuleIndices;
	std::string appliedTreeMeshSearch;
	std::string treeWindSaveStatus;
	std::size_t filteredTreeRuleCount = static_cast<std::size_t>(-1);
	bool treeWindSaveSucceeded = false;

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
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 64);

	struct alignas(16) VanillaPointLightData
	{
		uint32_t pointLightFlags[8];
	};
	STATIC_ASSERT_ALIGNAS_16(VanillaPointLightData);
	static_assert(sizeof(VanillaPointLightData) == 32);

	ConstantBuffer* vanillaPointLightCB = nullptr;

	// Keep these mirrored with GrassWindSpring::TEXTURE_SIZE and WORLD_SIZE.
	static constexpr uint32_t kGrassWindSpringTextureSize = 128;
	static constexpr float kGrassWindSpringWorldSize = 32768.0f;

	struct alignas(16) GrassWindSpringData
	{
		float2 fieldMinimum;
		float2 previousFieldMinimum;
		float fieldHeight;
		float frameTime;
		float responseRadians;
		float maximumTiltRadians;
		float sensitivity;
		float springFrequency;
		float springDamping;
		uint32_t initialize;
		uint32_t fieldAvailable;
		float3 padding;
	};
	STATIC_ASSERT_ALIGNAS_16(GrassWindSpringData);

	ConstantBuffer* grassWindSpringCB = nullptr;
	Texture2D* grassWindSpringResponseTextures[2] = {};
	Texture2D* grassWindSpringVelocityTextures[2] = {};
	winrt::com_ptr<ID3D11SamplerState> grassWindSpringSampler;
	uint32_t grassWindSpringTextureIndex = 0;
	float2 grassWindSpringFieldMinimum{};
	float2 previousGrassWindSpringFieldMinimum{};
	bool grassWindSpringInitialized = false;
	bool grassWindSpringFieldAvailable = false;
	Util::LazyShader<ID3D11ComputeShader> grassWindSpringCS;

	virtual void DrawSettings() override;
	/** @brief Exposes live tree tuning counts to devbench. */
	virtual json GetDiagnostics() override;
	/** @brief Exposes the runtime-only wind visualization toggle to devbench. */
	virtual json GetRuntimeFlags() override;
	virtual bool SetRuntimeFlag(std::string_view a_name, bool a_value) override;
	/** @copydoc Feature::RegisterUxActions */
	virtual void RegisterUxActions() override;
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
	/** @brief Advances and binds the persistent grass wind spring field once per frame. */
	void UpdateGrassWindSpring();
	/** @brief Releases the cached spring compute shader for file-watcher recompilation. */
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;

	PerFrameData GetCommonBufferData() const;
	/** @brief Enables or releases the runtime-only wind conditions used for tree tuning. */
	void SetTreeWindTestEnabled(bool a_enabled);
	/** @return Whether weather speed or the persistent debug-speed controls drive the shared field. */
	[[nodiscard]] bool ShouldUseRealWindSpeed() const { return !treeWindTest.enabled && windFieldUseRealSpeed; }
	/** @return Runtime test speed while testing, otherwise the existing debug-speed override. */
	[[nodiscard]] float GetEffectiveWindOverrideSpeed() const { return treeWindTest.enabled ? treeWindTest.speed : windFieldOverrideSpeed; }
	/** @return Runtime test gust scale while testing, otherwise the persistent Wind Field value. */
	[[nodiscard]] float GetEffectiveWindGustScale() const { return treeWindTest.enabled ? treeWindTest.gustScale : settings.windFieldGustScale; }
	/** @return Runtime test gust amplitude while testing, otherwise the persistent Wind Field value. */
	[[nodiscard]] float GetEffectiveWindGustAmplitude() const { return treeWindTest.enabled ? treeWindTest.gustAmplitude : settings.windFieldGustAmplitude; }
	/** @return Runtime test gust advection while testing, otherwise the persistent Wind Field value. */
	[[nodiscard]] float GetEffectiveWindGustAdvectionMultiplier() const { return treeWindTest.enabled ? treeWindTest.gustAdvectionMultiplier : settings.windFieldGustAdvectionMultiplier; }
	void UpdateVanillaPointLightData(RE::BSRenderPass* a_pass, uint32_t a_lightCount);
	void DrawDepthOfFieldSettings();
	/** Draws water tuning controls. */
	void DrawWaterSettings();
	void DrawVanillaBloomSettings();
	void DrawTreeWindTestSettings();
	void DrawTreeMeshSettings();
	void InstallDepthOfFieldHooks();

	static void SanitizeDepthOfFieldSettings(DepthOfFieldSettings& a_settings);
	static void SanitizeDepthOfFieldOverride(DepthOfFieldOverride& a_override);
	/** Clamps water controls before serialization or GPU upload. */
	static void SanitizeWaterSettings(WaterSettings& a_settings);

	struct Hooks;
};
