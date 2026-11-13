#pragma once

#include "Feature.h"
#include "Features/DX12NativeScene/NativeCharacterObservation.h"
#include "Features/DX12NativeScene/PreparedScene.h"
#include "Features/DX12NativeScene/RasterCapture.h"
#include "Features/DX12NativeScene/RuntimeGeometryCacheWorker.h"
#include "Features/DX12NativeScene/RuntimeGeometryCapture.h"
#include "Features/DX12NativeScene/RuntimeGeometryPreparation.h"
#include "Features/DX12NativeScene/ShadowTrace.h"
#include "Features/DX12NativeScene/SubcellStreamingManager.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include "TruePBR/InstanceCoverage.h"
#include "TruePBR/VerifiedBridge.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>

/** @brief Loads prepared geometry and publishes its completed DX12 acceleration scene. */
namespace RE
{
	class BSGeometry;
}
namespace ShadowCasterManager
{
	struct NativeCasterFrame;
}

struct DX12NativeScene final : Feature
{
	struct Settings
	{
		bool Enabled = true, ShadowMask = true;
		// Native features follow one DX12 pipeline ownership decision, never per-effect validity gates.
		bool NativePBR = true, NativeLLF = true, NativeSCM = true;
		uint32_t BudgetMB = 0, FrameBudgetMB = 0, MaterialBatchMB = 16;  // Retired manual caps; zero means dynamic DXGI.
		uint32_t BudgetPolicyVersion = 1;                                // Migrate legacy 512 MiB default to auto
		uint32_t GlobalDXGIHeadroomPercent = DX12Native::VideoMemoryBudget::kDefaultHeadroomPercent;
		uint32_t SubdivisionMode = 0;         // 0 full; 1 auto after estimate; 2 forced companion prep.
		bool AutoGridMemoryScreening = true;  // false: automatic mode uses grid directly.
		uint32_t SubcellSize = 4096;          // 2048..16384 in 1024 unit steps
		bool BuildCandidateGridTLAS = false;  // Disabled-by-default isolated DXR test.
		bool UseGridForNativeDXR = false;     // Opt-in only, require matching TLAS/material generation.
		bool UseLivePBR = false;              // Legacy config field; NativePBR now follows DX12 ownership.
		bool ImportLoosePBR = false;          // Separate opt-in for bounded loose-file DDS residency.
		bool ImportArchivePBR = false;        // Further opt-in: archive resource manager fallback.
		bool RuntimeOnlyMaterials = false;    // Optional manifest-free runtime texture acquisition.
		bool CaptureRuntimeGeometry = false;  // Explicit static-geometry debug.
		bool CaptureSCMRigidCasters = true;   // Bounded native acquisition from SCM's cull data.
		bool CacheRuntimeGeometry = false;    // Optional background persistence and menu CPU prewarm.
		// Third-person character discovery only. It never changes DXR ownership.
		bool ObserveNativeCharacters = false;
		bool CharacterPlayerOnly = false;
		// Capture owned bind inputs and stage them in DX12 without TLAS use.
		bool CharacterStageGeometryDX12 = false;
		bool CharacterRunSkinningDX12 = false;            // Isolated pose-parity candidate.
		bool CharacterValidateSkinningDX12 = false;       // Bounded GPU readback diagnostics only.
		bool CharacterBuildIsolatedBLASDX12 = false;      // No production TLAS/ray ownership.
		bool CharacterBuildIsolatedTLASDX12 = false;      // Single instance; diagnostic only.
		bool CharacterRunIsolatedRayQueryDX12 = false;    // One diagnostic ray, no shadows.
		bool CharacterAccumulatePartBLAS = false;         // Bounded independent multi-part construction.
		bool CharacterIncludeRigidAttachments = false;    // Reuse static BLAS with observed world transforms.
		bool CharacterMultiPartCasterVisibility = false;  // Extra opt-in after per-part ray proof.
		bool CharacterShadowCasterVisibility = false;     // Opt-in; secondary light rays only.
		uint32_t CharacterCasterMaxPoseAgeFrames = 12;    // Bound TLAS reuse across BLAS updates.
		uint32_t CharacterMaxActors = 16, CharacterObservationPeriod = 8;
		uint32_t CharacterMeshCaptureBatch = 2;    // 1..4 bounded CPU copies/sample.
		uint32_t CharacterBLASIntervalFrames = 8;  // Bound experimental GPU AS work.
		// Independent early DX12 shadow-mask parameters from reference PR #9.
		bool ShadowAllLights = true, ShadowStochastic = true, ShadowDistanceFade = true;
		uint32_t ShadowLightIndex = 0, ShadowNormalInfluence = 2;
		uint32_t ShadowTraceDensityMode = 0;  // 0..7; all trace densities retain full-size raw masks.
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
		std::shared_ptr<const DX12Native::NativeSceneLightBake> bakedLights;
	};
	// NativeLLF unifies prepared and live lights for native DXR with one shared capacity.
	struct NativeLightBatch
	{
		std::array<DX12Native::PreparedPointLight, DX12Native::kMaximumNativePointLights> lights{};
		uint32_t count = 0, observed = 0, prepared = 0, matched = 0, added = 0, baked = 0, clustered = 0, omitted = 0, overflow = 0;
	};
	struct SceneFrame
	{
		uint32_t frame = 0;
		std::shared_ptr<const DX12Native::RasterFrame> raster;
		std::optional<PublishedScene> prepared;
		std::shared_ptr<const DX12Native::ShadowFrame> shadow;
		// Same-frame classified raster casters for NativeSCM consumers (no engine pointers).
		std::shared_ptr<const ShadowCasterManager::NativeCasterFrame> nativeCasters;
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
	void ObserveSCMRigidCaster(RE::BSGeometry& geometry);  // Data only; DX12 evaluates rays.
	void OnRuntimeDisabled() override;
	void OnSceneTransitionReset(bool a_open) override;
	void GameLoaded() override;
	/** @brief Request a safe retry of a newly published CELL on the scene thread. */
	void NotifyPreparedCellPublished(std::string_view plugin, uint32_t localID);
	void ClearShaderCache() override { reloadRequested = true; }
	json GetDiagnostics() override;
	void RegisterUxActions() override;
	/** @brief Requests only prepared geometry and the independent early shadow producer.
	 *  Does not enable Native Lighting or alter its settings. */
	void RequestShadowProducerForComposite()
	{
		requestedEnabled = 1;
		requestedShadow = 1;
		shadowResetRequested = true;
	}
	/** @brief The selected DX12 pipeline owns native materials, lights and casters.
	 *  Not dependent on material/scene/effect validity. */
	bool OwnsNativeFeatures() const;
	/** @brief True only when this exact static reference/shape exists in the active native TLAS. */
	bool IsPreparedStaticCaster(RE::BSGeometry& geometry) const;
	/** @brief Retains the scene graphics session for DX12-only consumers. */
	std::optional<GraphicsBackend> GetGraphicsBackend() const { return loaded && settings.Enabled && !transitionOpen ? backend : std::nullopt; }
	/** @brief Retains a completed scene matching the current cell and revision. */
	std::optional<PublishedScene> GetPublishedScene() const;
	/** @brief Live LLF illumination source in a native-owned frame; empty is not a legacy fallback. */
	std::optional<NativeLightBatch> GetNativeLLFLights(uint32_t a_frame) const;
	/** @brief Retains SCM's finished same-frame caster classification generation. */
	std::shared_ptr<const ShadowCasterManager::NativeCasterFrame> GetNativeSCMFrame(uint32_t a_frame) const;
	/** @brief Returns the available current-frame outputs without substituting stale resources. */
	std::optional<SceneFrame> GetSceneFrame(uint32_t a_frame) const;
	/** @brief Retains the current raster snapshot through the consumer GPU work. */
	std::shared_ptr<const DX12Native::RasterFrame> GetRasterFrame(uint32_t a_frame) const { return loaded && settings.Enabled && !transitionOpen ? raster.Get(a_frame) : nullptr; }
	/** @brief Eligible third-person character caster for independent native
     * visibility consumers. This does not require the separate shadow-mask
     * toggle, and does not promote characters to prepared receivers.
     */
	std::shared_ptr<const DX12Native::CharacterIsolatedTLAS>
	GetCharacterCasterForVisibility(uint32_t a_frame) const;
	/** @brief Returns a matching prepared-scene shadow mask and its completion fence. */
	std::shared_ptr<const DX12Native::ShadowFrame> GetShadowMask(uint32_t a_frame) const;
	/** @brief Retains a borrowed shadow mask until the consumer GPU fence completes. */
	bool RetainShadowMask(const std::shared_ptr<const DX12Native::ShadowFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value) { return shadow.RetainThrough(a_frame, a_fence, a_value); }
	/** @brief Retains the borrowed raster frame until the consumer GPU fence completes. */
	bool RetainRasterFrame(const std::shared_ptr<const DX12Native::RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value) { return raster.RetainThrough(a_frame, a_fence, a_value); }

private:
	void ResetScene();
	uint64_t SceneMemoryLimit(uint64_t trackedBytes = 0);
	void PublishDiagnostics();
	void AdvanceCoverage();
	void AdvanceNativeSCMCoverage();
	struct CoverageTask;
	struct NativeSCMCoverageTask;
	void DispatchShadow(const char* a_stage);
	std::optional<GraphicsBackend> backend;
	DX12Native::PreparedScene prepared;
	DX12Native::SubcellStreamingManager subcellManager;
	DX12Native::AccelerationWorker candidateGridAcceleration;
	DX12Native::ShadowBindingWorker candidateGridBindings;
	std::shared_ptr<const DX12Native::PreparedCell> lastSubmittedGridCell;
	// Atomic publication unit: keep all three resources together through
	// candidate builds, GPU fences, and later 3x3 recentering.
	std::optional<PublishedScene> stableGridPublished;
	DX12Native::SpatialSubcells::Coordinate stableGridCenter{};
	uint64_t gridHandoffs = 0;
	bool autoGridDecisionLatched = false;
	std::shared_ptr<const DX12Native::TopLevel> requestedCandidateMaterialScene;
	uint64_t candidateMaterialAllowance = 0;
	DX12Native::RuntimeGeometryMailbox runtimeGeometry;
	DX12Native::NativeCharacterObservation nativeCharacters;
	// Immutable completed-TLAS identity index is published atomically for the draw hook.
	std::atomic<std::shared_ptr<const DX12Native::PreparedRuntimeCoverage>> runtimePreparedCoverage;
	std::shared_ptr<const DX12Native::TopLevel> runtimeCoverageSource;
	std::shared_ptr<NativeSCMCoverageTask> runtimeCoverageTask;
	std::atomic<uint64_t> runtimePreparedSkipped{ 0 };
	uint64_t runtimePreparedPruned = 0;
	DX12Native::RuntimeGeometryCacheWorker runtimeGeometryCache;
	DX12Native::RuntimeGeometryPreparation runtimeGeometryPrepare;
	// The same production acceleration worker receives composed current-scene
	// generations. Retain the last usable TLAS/material pair during rebuilds.
	std::shared_ptr<const DX12Native::TopLevel> currentSceneStableAcceleration;
	std::shared_ptr<const DX12Native::ShadowBindings> currentSceneStableBindings;
	uint64_t currentSceneSubmittedBatch = 0;
	DX12Native::VideoMemoryBudget preparedSceneVideoMemory;
	DX12Native::VideoMemoryBudget::Snapshot preparedSceneBudgetStatus{};
	DX12Native::VideoMemoryBudget runtimeGeometryVideoMemory;
	DX12Native::VideoMemoryBudget::Snapshot runtimeGeometryBudgetStatus{};
	uint64_t runtimeGeometryActiveBytes = 0, runtimeGeometryAllowedBytes = 0;
	std::atomic<bool> runtimeGeometryCaptureActive{ false };
	std::mutex runtimeGeometryBudgetMutex;
	uint32_t runtimeGeometryBudgetFrame = UINT32_MAX, runtimeGeometryFrameBytes = 0;
	DX12Native::RasterCapture raster;
	DX12Native::ShadowTrace shadow;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::ShadowBindingWorker materialBindings;
	std::shared_ptr<const DX12Native::TopLevel> materialRequestBudgetScene;
	uint64_t materialRequestBudgetBytes = 0;
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
	std::atomic<uint64_t> pendingPreparedCellReload{ 0 };
	std::atomic<int> requestedEnabled{ -1 }, requestedShadow{ -1 };
	std::atomic<uint32_t> requestedBudget{ UINT32_MAX };
	std::atomic<uint32_t> requestedFrameBudget{ UINT32_MAX };
	std::atomic<uint32_t> requestedMaterialBatch{ 0 };
	double previousPlayerX = 0.0, previousPlayerY = 0.0;
	bool hasPreviousPlayer = false;
	uint64_t shadowControlsRevision = 1;
	uint64_t shadowOpaqueRetryAttempts = 0, shadowOpaqueRetryRecovered = 0;
	mutable std::mutex settingsMutex, diagnosticsMutex;
	json diagnostics = json::object();
	bool submitted = false;
	bool transitionOpen = false;
};
