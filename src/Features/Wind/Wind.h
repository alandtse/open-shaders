#pragma once

#include "Feature.h"
#include "Features/Wind/Grass/GrassWindState.h"
#include "Features/Wind/Runtime/WindRuntimeState.h"
#include "Features/Wind/Settings/WindSettings.h"
#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/Trees/TreeWindState.h"
#include "Features/Wind/UI/WindUIState.h"
#include "Features/Wind/WindField.h"
#include "I18n/I18n.h"
#include <OpenWind/API.h>
#include <OpenWind/RendererAPI.h>
#include <OpenWind/UIAPI.h>

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

/** Per-frame contribution Wind makes to State::PermutationCB; see State::UpdatePermutationBuffer. */
struct WindPermutationContribution
{
	float windIntensityOverride;
	uint32_t overrideWindIntensity;
	uint32_t enableGrassWindSpringBend;
	float treeTransientWindInfluenceDefault;
	float treeLeafTransientWindInfluenceDefault;
	float treeLeafTransientFlutterMaximumDefault;
	float treeTransientMaximumBendMultiplierDefault;
	float trunkWindBendSensitivity;
	float treeLeafBaseWindFlutterGain;
	uint32_t enableAmbientGrassWind;
	float grassWindSensitivity;
	float grassWindBendProfile;
	float grassWindCompressionToBend;
	float grassWindFlutterStrength;
	float grassWindFlutterFrequency;
};

/** Per-frame contribution Wind makes to State::SharedDataCB; see State::UpdateSharedData. */
struct WindSharedData
{
	WindField::WindTuning tuning;
	float4 ambient;
	float4 previousAmbient;
	WindField::Field current;
	WindField::Field previous;
	WindField::Field transition;
	WindField::Field previousTransition;
	float4 transitionData;
	float4 springDebug;
	std::array<uint32_t, 4> activeCounts;
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> transientImpulses;
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> previousTransientImpulses;
};

struct Wind : Feature
{
	/** @brief Wind components sampled from an immutable published simulation state. */
	struct PublishedWindSample
	{
		float3 baseVelocity{};
		float3 gustVelocity{};
		float3 transientVelocity{};
		float3 finalVelocity{};
		float3 windExcludingHavokImpulses{};
		float ambientGust{};
		float transientIntensity{};
		uint64_t frameId{};
	};

	virtual std::string GetName() override { return "Wind"; }
	virtual std::string GetDisplayName() override { return T("feature.wind.name", "Wind"); }
	virtual std::string GetShortName() override { return "Wind"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFoliage; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.wind.description", "Shared ambient, tree, grass, and transient wind controls."),
			{ T("feature.wind.key_feature_1", "Procedural ambient gust field"),
				T("feature.wind.key_feature_2", "Tree and grass response tuning"),
				T("feature.wind.key_feature_3", "Transient wind impulses") } };
	}

	using Settings = WindSettings;
	using SettingsPage = WindSettingsPage;
	using PerFrameData = WindPerFrameData;
	using GrassWindSpringFieldData = ::GrassWindSpringFieldData;
	using GrassWindSpringData = ::GrassWindSpringData;
	using WindFieldDebugView = ::WindFieldDebugView;
	using RuntimeWindTest = ::RuntimeWindTest;

	Settings settings;
	WindUIState uiState;
	WindRuntimeState runtimeState;
	GrassWindState grassState;
	TreeWindState treeState;

	float windFieldFrameTime = 0.0f;
	float windFieldAmbientSpeed = 0.0f;
	float windFieldAdvectionSpeed = 0.0f;
	float windFieldGustTravelDistance = 0.0f;
	float previousWindFieldGustTravelDistance = 0.0f;
	float windFieldTravelDelta = 0.0f;
	float3 ambientWindVelocity = {};
	float3 windFieldSelectedVelocity = {};
	float3 previousWindFieldSelectedVelocity = {};
	WindField::Field windFieldCurrent{};
	WindField::Field previousWindFieldCurrent{};
	WindField::Field windFieldTransition{};
	WindField::Field previousWindFieldTransition{};
	float windFieldTransitionBlend = 1.0f;
	float previousWindFieldTransitionBlend = 1.0f;
	bool windFieldTransitionActive = false;
	float windFieldSelectedSpeed = 0.0f;
	WindField::WindTuning windFieldTuning{};

	/** @copydoc Feature::DrawSettings */
	virtual void DrawSettings() override;
	virtual json GetDiagnostics() override;
	virtual json GetRuntimeFlags() override;
	virtual bool SetRuntimeFlag(std::string_view a_name, bool a_value) override;
	virtual void RegisterUxActions() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual bool HasScopedDefaultSettings() const override { return true; }
	virtual void RestoreCurrentPageDefaultSettings() override;
	virtual bool HasScopedOverrideSettings() const override { return true; }
	virtual bool ReapplyCurrentPageOverrideSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void OnSceneTransitionReset(bool a_opening) override;
	virtual bool WantsRenderPassHook() const override { return true; }
	virtual std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* a_pass) override;
	/** @brief Advances the shared wind field and transient impulses for the current frame. */
	virtual void Reset() override;

	[[nodiscard]] PerFrameData GetCommonBufferData() const;
	[[nodiscard]] WindPermutationContribution GetPermutationContribution() const;
	[[nodiscard]] WindSharedData GetSharedWindData() const;
	/** Samples the Open Wind provider without advancing its state. */
	[[nodiscard]] bool SamplePublishedWind(std::span<const float3> a_worldPositions,
		std::span<PublishedWindSample> a_samples) const noexcept;
	void SetTreeWindTestEnabled(bool a_enabled);
	void UpdateGrassWindSpring(bool a_compute = false);
	void UpdateTreeWindSpring();
	void RecreateGrassWindSpringTextures(uint32_t a_qualityIndex, uint32_t a_textureSize);
	[[nodiscard]] ID3D11ShaderResourceView* GetGrassWindSpringDebugSRV() const;

private:
	bool IsTreeBendRenderPass(const RE::BSRenderPass* a_pass) const;
	const OpenWind::RendererAPI* provider{};
	std::atomic<const OpenWind::API*> samplingAPI{};
	const OpenWind::UIAPI* uiProvider{};
	std::unique_ptr<OpenWind::RendererFrame> providerFrame;
	uint64_t providerResetId{};
	json uiSettingsDocument = json::object();
	json uiControlsDocument;
	json pendingProviderSettings;
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> transientWindImpulses{};
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> previousTransientWindImpulses{};
	uint32_t activeTransientWindImpulseCount{};
	uint32_t previousActiveTransientWindImpulseCount{};
	void ResetSpringHistory();
	void ApplyProviderFrame(const OpenWind::RendererFrame& a_frame);
	void CommitProviderSettings();
	void RefreshProviderSettings();
	[[nodiscard]] bool ReadProviderSettings(json& a_document) const;
	[[nodiscard]] bool RunProviderAction(const json& a_action);
	[[nodiscard]] json RunProviderCommand(const json& a_request) const;
	[[nodiscard]] const OpenWind::AuthoringHost& GetAuthoringHost();
	void DrawWindEffectControls();
	static void SanitizeSettings(Settings& a_settings);
	static void SanitizeGrassWindSettings(Settings& a_settings);
	static uint32_t SanitizeGrassWindSpringTextureSize(uint32_t a_textureSize);
	[[nodiscard]] uint32_t GetTransientFieldMask() const;
	void DrawWindFieldSettings();
	void DrawWindEffectsSettings();
	void SpawnDebugWindEffects();
	void DrawTreeSettings();
	void DrawTreeWindTestSettings();
	void DrawTreeMeshSettings();
	void DrawTreeMeshConflictWarning();
	void DrawTreeGlobalOverrideSettings();
	void DrawTreeMeshRuleControls();
	void DrawTreeMeshRulesTable();
	void ResetGrassWindSettings();
	void DrawGrassTransientSettings();
	void DrawGrassWindSettings();
	void SetupGrassWindResources();
	void SetupTreeWindResources();
	void RecreateTreeWindSpringTextures(uint32_t a_qualityIndex);
};
