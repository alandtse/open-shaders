#pragma once

#include "Feature.h"
#include "Features/DX12NativeScene/PreparedScene.h"
#include "Features/DX12NativeScene/RuntimeGeometryCapture.h"
#include "Features/DX12NativeScene/RasterCapture.h"
#include "Features/DX12NativeScene/ShadowTrace.h"
#include "TruePBR/InstanceCoverage.h"
#include "TruePBR/VerifiedBridge.h"
#include <atomic>
#include <optional>
#include <mutex>

/** @brief Loads prepared geometry and publishes its completed DX12 acceleration scene. */
namespace RE { class BSGeometry; }

struct DX12NativeScene final : Feature
{
	struct Settings
	{
		bool Enabled = true, ShadowMask = true;
		uint32_t BudgetMB = 512, FrameBudgetMB = 0, MaterialBatchMB = 16;
		bool UseLivePBR = false; // Experimental opt-in; do not enable before runtime validation.
        bool ImportLoosePBR = false; // Separate opt-in for bounded loose-file DDS residency.
        bool ImportArchivePBR = false; // Further opt-in: archive resource manager fallback.
        bool RuntimeOnlyMaterials = false; // Optional manifest-free runtime texture acquisition.
        bool CaptureRuntimeGeometry = false; // Static CPU-backed meshes only; no GPU readbacks.
		// Independent early DX12 shadow-mask parameters from reference PR #9.
		bool ShadowAllLights = true, ShadowStochastic = true, ShadowDistanceFade = true;
		uint32_t ShadowLightIndex = 0, ShadowNormalInfluence = 2;
		uint32_t ShadowTraceDensityMode = 0; // 0..7; all trace densities retain full-size raw masks.
		float ShadowEmitterRadius = 0.2f;
		float ShadowReachMultiplier = 2.0f, ShadowFalloffFraction = 1.0f, ShadowFalloffExponent = 0.6f;
	} settings;
	struct PublishedScene
	{
		std::shared_ptr<const DX12Native::PreparedCell> cell;
		std::shared_ptr<const DX12Native::TopLevel> acceleration;
		std::shared_ptr<const DX12Native::ShadowBindings> materials;
		uint32_t cellID = 0;
		uint64_t revision = 0;
	};
	struct SceneFrame
	{
		uint32_t frame = 0;
		std::shared_ptr<const DX12Native::RasterFrame> raster;
		std::optional<PublishedScene> prepared;
		std::shared_ptr<const DX12Native::ShadowFrame> shadow;
	};
	std::string GetName() override { return "DX12 Native Scene"; }
	std::string GetShortName() override { return "DX12NativeScene"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	void DrawSettings() override;
	void LoadSettings(json& a_json) override;
	void SaveSettings(json& a_json) override;
	void RestoreDefaultSettings() override;
	void EarlyPrepass() override;
	void OnBeforeShadowMapRendering() override { DispatchShadow("before-raster-shadowmaps"); }
	void OnWorldRenderBegin() override { DispatchShadow("world-render-begin"); }
	void Prepass() override { DispatchShadow("before-opaque"); }
	void OnOpaqueRasterReady(const OpaqueRasterInputs& a_inputs) override;
    void ObserveRuntimeGeometry(RE::BSGeometry* a_geometry, const std::string& a_reference, const std::string& a_shape);
	void OnRuntimeDisabled() override;
	void OnSceneTransitionReset(bool a_open) override;
	void GameLoaded() override;
	void ClearShaderCache() override { reloadRequested = true; }
	json GetDiagnostics() override;
	void RegisterUxActions() override;
	/** @brief Requests only prepared geometry and the independent early shadow producer.
	 *  Does not enable Native Lighting or alter its settings. */
	void RequestShadowProducerForComposite() {
		requestedEnabled = 1;
		requestedShadow = 1;
		shadowResetRequested = true;
	}
	/** @brief Retains the scene graphics session for DX12-only consumers. */
	std::optional<GraphicsBackend> GetGraphicsBackend() const { return loaded && settings.Enabled && !transitionOpen ? backend : std::nullopt; }
	/** @brief Retains a completed scene matching the current cell and revision. */
	std::optional<PublishedScene> GetPublishedScene() const;
	/** @brief Returns the available current-frame outputs without substituting stale resources. */
	std::optional<SceneFrame> GetSceneFrame(uint32_t a_frame) const;
	/** @brief Retains the current raster snapshot through the consumer GPU work. */
	std::shared_ptr<const DX12Native::RasterFrame> GetRasterFrame(uint32_t a_frame) const { return loaded && settings.Enabled && !transitionOpen ? raster.Get(a_frame) : nullptr; }
	/** @brief Returns a matching prepared-scene shadow mask and its completion fence. */
	std::shared_ptr<const DX12Native::ShadowFrame> GetShadowMask(uint32_t a_frame) const;
	/** @brief Retains a borrowed shadow mask until the consumer GPU fence completes. */
	bool RetainShadowMask(const std::shared_ptr<const DX12Native::ShadowFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value) { return shadow.RetainThrough(a_frame, a_fence, a_value); }
	/** @brief Retains the borrowed raster frame until the consumer GPU fence completes. */
	bool RetainRasterFrame(const std::shared_ptr<const DX12Native::RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value) { return raster.RetainThrough(a_frame, a_fence, a_value); }

private:
	void ResetScene();
	void PublishDiagnostics();
	void AdvanceCoverage();
	struct CoverageTask;
	void DispatchShadow(const char* a_stage);
	std::optional<GraphicsBackend> backend;
	DX12Native::PreparedScene prepared;
    DX12Native::RuntimeGeometryMailbox runtimeGeometry;
    std::atomic<bool> runtimeGeometryCaptureActive{ false };
    std::mutex runtimeGeometryBudgetMutex;
    uint32_t runtimeGeometryBudgetFrame = UINT32_MAX, runtimeGeometryFrameBytes = 0;
	DX12Native::RasterCapture raster;
	DX12Native::ShadowTrace shadow;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::ShadowBindingWorker materialBindings;
	std::weak_ptr<const DX12Native::ShadowBindings> retiredMaterials;
	// Immutable recipe/hint snapshot retained through the asynchronous binding worker.
	std::shared_ptr<const NativePBR::LiveBindingSource> livePbrSource;
	uint64_t livePbrRevision = 0;
	uint32_t livePbrLastCaptureFrame = 0;
	std::shared_ptr<CoverageTask> coverageTask;
	std::optional<NativePBR::InstanceCoverage> coverageResult;
	std::atomic<bool> coverageRequested{ false };
	uint64_t coverageRevision = 0;
	bool coverageFailed = false;
	mutable std::mutex coverageMutex;
	std::string failure, shadowStatus, shadowStage;
	uint64_t revision = 0;
	uint32_t cellID = 0;
	std::atomic<bool> resetRequested{ false }, reloadRequested{ false }, shadowResetRequested{ false };
	std::atomic<int> requestedEnabled{ -1 }, requestedShadow{ -1 };
	std::atomic<uint32_t> requestedBudget{ 0 };
	std::atomic<uint32_t> requestedFrameBudget{ UINT32_MAX };
	std::atomic<uint32_t> requestedMaterialBatch{ 0 };
	uint64_t shadowControlsRevision = 1;
	uint64_t shadowOpaqueRetryAttempts = 0, shadowOpaqueRetryRecovered = 0;
	mutable std::mutex settingsMutex, diagnosticsMutex;
	json diagnostics = json::object();
	bool submitted = false;
	bool transitionOpen = false;
};
