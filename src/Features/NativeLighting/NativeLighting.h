#pragma once

#include "Feature.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include <atomic>
#include <mutex>

/** @brief Produces independent DX12 direct diffuse and specular contributions. */
struct NativeLightingFeature final : Feature
{
	struct Settings
	{
		bool Enabled = false, Diffuse = true, Specular = true, EarlyShadowCull = true, Debug = false, CompareCulling = false;
		float DiffuseIntensity = 1.0f, SpecularIntensity = 1.0f;
		// Ported from reference PR #9; native direct owns these independently.
		float DirectStrength = 2.0f, EmissionStrength = 1.0f;
		float PointLightRadiusMultiplier = 4.0f, PointLightFalloffFraction = 0.1f, PointLightFalloffExponent = 1.0f;
		float ShadowStrength = 1.0f, EmitterRadiusMultiplier = 1.0f;
		uint32_t NormalInfluence = 2, SelectedDirectLight = 0;
		uint32_t TraceDensityMode = 0; // 0..7, native-resolution sparse ray eligibility.
		bool StochasticShadows = false, UseAllPreparedLights = true;
		uint32_t BudgetMB = 0; // 0 = dynamic DXGI local-memory-aware frame pool.
		bool PersistLab = false;
	};
	struct ValidityLab
	{
		bool DepthAgreement = true;
		bool DiagnosticDepth = false;
		float AbsoluteTolerance = 5.0f;
		float RelativeTolerance = 0.00001f;
		uint32_t MaterialPolicy = 0;
		bool RequireMaskAuthority = true;
		bool AssumeMaskAuthoritative = false;
		bool ObserveMask = false;
		bool RunWithoutLighting = false;
		bool ReferenceVisibility = true;
		bool ReferencePrimaryTraversal = true;
		float MaskThreshold = 0.0f;
		float MaskDistanceTolerance = 2.0f;
	};
	struct Contribution
	{
		enum class Output : size_t { Diffuse, Specular, Validity, ReceiverGuide, Debug, ComparisonDiffuse, ComparisonSpecular, EffectiveValidity, RejectionReason, LabGuide };
		std::array<winrt::com_ptr<ID3D12Resource>, 10> textures;
		winrt::com_ptr<ID3D12Resource> counters, counterReset, counterReadback, lights, constants;
		winrt::com_ptr<ID3D12DescriptorHeap> heap;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> readyFence;
		GraphicsBackend backend;
		DX12NativeScene::SceneFrame scene;
		std::weak_ptr<const DX12Native::ShadowBindings> descriptorBindings;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
		uint64_t readyValue = 0, bytes = 0, sceneRevision = 0;
		uint64_t validityRevision = 0;
		uint32_t frame = 0, width = 0, height = 0, enabledLobes = 0, traceDensityMode = 0;
		float diffuseIntensity = 0, specularIntensity = 0;
		bool countersRead = false, comparisonEnabled = false, geometryOnly = false, maskAvailable = false, maskExperimental = false;
	};
	std::string GetName() override { return "Native Lighting"; }
	std::string GetShortName() override { return "NativeLighting"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	void DrawSettings() override;
	void LoadSettings(json& a_json) override;
	void SaveSettings(json& a_json) override;
	void RestoreDefaultSettings() override;
	void OnOpaqueRasterReady(const OpaqueRasterInputs&) override;
	void OnRuntimeDisabled() override { resetRequested = true; }
	void OnSceneTransitionReset(bool a_open) override;
	void GameLoaded() override { resetRequested = true; }
	void ClearShaderCache() override { reloadRequested = true; }
	json GetDiagnostics() override;
	void RegisterUxActions() override;
	/** @brief Borrows current-frame DX12 outputs until consumer completion. */
	std::shared_ptr<const Contribution> GetContribution(uint32_t a_frame) const;
	/** @brief Retains a contribution through a downstream GPU completion fence. */
	bool RetainContribution(const std::shared_ptr<const Contribution>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
	/** @brief NativeSCM queries direct-light selection through the owning feature,
	 *  without reading NativeLighting's private settings from external code. */
	bool SelectsLightForNativeShadows(uint32_t index) const
	{
		std::scoped_lock lock(settingsMutex);
		return loaded && settings.Enabled &&
			(settings.UseAllPreparedLights || settings.SelectedDirectLight == index);
	}
	~NativeLightingFeature();

private:
	bool Initialize(const GraphicsBackend& a_backend);
	void Dispatch(DX12NativeScene::SceneFrame a_scene, const Settings& a_settings, const ValidityLab& a_lab, uint64_t a_revision);
	void Retire();
	void Reset();
	void PublishDiagnostics();
	Settings settings;
	ValidityLab lab;
	uint64_t validityRevision = 1;
	mutable std::mutex settingsMutex;
	std::mutex diagnosticsMutex;
	json diagnostics = json::object();
	GraphicsBackend backend;
	winrt::com_ptr<ID3D12Fence> completion;
	winrt::com_ptr<ID3D12RootSignature> root;
	winrt::com_ptr<ID3D12PipelineState> pipeline;
	std::array<std::shared_ptr<Contribution>, 4> frames;
	std::shared_ptr<Contribution> current;
	std::array<uint64_t, 27> totals{};
	// Derived after the existing GPU candidate counter readback; no extra per-pixel atomic.
	uint64_t skippedPixels = 0;
	std::string status;
	uint64_t nextFence = 0, submissions = 0, completions = 0, counterReadFailures = 0, comparisonFrames = 0, busyDeferrals = 0;
	DX12Native::VideoMemoryBudget videoBudget;
	std::atomic<bool> resetRequested{ false }, reloadRequested{ false }, transitionOpen{ false }, active{ false };
	bool failed = false;
};
