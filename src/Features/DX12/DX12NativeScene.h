#pragma once

#include "../OverlayFeature.h"
#include "DX12NativeAcceleration.h"
#include "DX12NativeLightWorker.h"
#include "DX12NativeRasterCapture.h"
#include "DX12NativeRuntimeCapture.h"
#include "DX12NativeShadowTrace.h"
#include "SCMNativePolicy.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/** @brief Independent early opaque raster capture for native DX12. */
struct DX12NativeScene final : OverlayFeature
{
	std::string GetName() override { return "DX12 Native Scene"; }
	std::string GetShortName() override { return "DX12NativeScene"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	bool IsInMenu() const override { return false; }
	bool IsInFeatureList() const override { return false; }
	bool SupportsVR() override { return false; }

	struct Settings
	{
		struct FullRTProfile
		{
			bool OutdoorShadowOffloading = false;
			uint32_t ShadowQualityMode = DX12Native::kShadowQualityQuality;
			bool DebugNativeLights = false;
			uint32_t RuntimeUnificationRadius = DX12Native::kDefaultRuntimeUnificationRadius;
			uint32_t PreparedExclusionRadius = DX12Native::kDefaultPreparedExclusionRadius;
			bool AdditivePreparedEmission = true;
			uint32_t LightObservationFrames = DX12Native::kDefaultLightObservationFrames;
		};
		bool OutdoorShadowOffloading = false;  // Off by default; vanilla raster shadows are unaffected.
		bool DebugNativeLights = false;
		FullRTProfile FullRT;
	};
	Settings settings;

	void DrawSettings() override;
	void DrawDevSettings();
	/** @brief The selected mode's exterior preparation and shadow contribution setting. */
	bool AllowsOutdoorContribution() const;
	/** @brief Comparison never admits native scene work, even for interiors. */
	bool AllowsNativeSceneWork() const;
	bool AllowsScenePreparation(bool exterior) const { return AllowsNativeSceneWork() && (!exterior || AllowsOutdoorContribution()); }
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;
	bool WantsRenderPassHook() const override;
	std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* pass) override;
	bool WantsPreShadowMapsHook() const override;
	void PreShadowMaps() override;
	/** @brief Starts asynchronous acceleration preparation for a menu-preloaded cell. */
	void PrepareMenuScene(std::shared_ptr<const DX12::Grid> grid);
	/** @brief Prepares a loading destination through the same acceleration worker without scene ownership. */
	void PrepareLoadingScene(std::shared_ptr<const DX12::Grid> grid);
	/** @brief Retires a whole-cell build when it cannot fit the current DXGI budget. */
	bool RetireBudgetDeferredWholeCell(uint64_t revision)
	{
		if (!acceleration.WholeCellBudgetDeferred(revision))
			return false;
		acceleration.Reset();
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		return true;
	}
	void LoadSettings(json& a_input) override;
	void SaveSettings(json& a_output) override;
	void RestoreDefaultSettings() override;
	bool WantsOpaqueRasterReadyHook() const override;
	void OnOpaqueRasterReady() override;
	/** @brief Observe CPU-backed opaque rigid geometry from SCM or normal render passes. */
	void ObserveSCMRigidCaster(RE::BSGeometry& geometry, bool requireStability = false);
	/** @brief SCM hands native-owned lights' static casters to RayQuery and retains movers. */
	bool OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const;
	/** @brief Frame-wide policy established before SCM scheduling. */
	DX12::SCMNativeMode GetFrameNativeMode() const;
	// Snapshot LLF/SCM point-light parameters without altering the DX11 light.
	void ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID, bool flickers,
		const RE::NiPoint3& restOrigin);
	void OnLoadingMenuEvent(bool opening) override;
	void OnSceneTransitionReset(bool a_opening) override;
	void OnRuntimeDisabled() override;

	/** @brief Raw mode-sized shadow output, independently fenced for Native Composite. */
	std::shared_ptr<const DX12Native::ShadowMaskFrame> GetShadowMask(uint32_t frame) const { return shadows.Get(frame); }

private:
	/** @brief Gates native scene work; never disables DX11 baseline shadows. */
	bool CanOffloadShadowsInCurrentCell() const;
	bool nativeSessionStarted = false;  // Render/main-thread lifecycle.
	std::atomic<bool> loadingResetPending{ false };
	void ApplyPendingLoadingReset();
	std::optional<GraphicsBackend> backend;
	DX12Native::RasterCapture raster;
	DX12Native::RuntimeGeometryCapture geometryCapture;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::ShadowMaskTrace shadows;
	// One immutable, ordered light selection shared by SCM and the RayQuery dispatch.
	std::atomic<std::shared_ptr<const std::vector<DX12::PreparedLight>>> shadowLights;
	std::atomic<uint32_t> shadowLightScope{ 0 };
	std::atomic<DX12::SCMNativeMode> frameNativeMode{ DX12::SCMNativeMode::Disabled };
	std::atomic<uint32_t> frameNativeModeID{ UINT32_MAX };
	std::atomic<bool> outdoorShadowOffloading{ false };  // Off by default; vanilla raster shadows are unaffected.
	std::atomic<bool> outdoorFullRT{ false };
	std::atomic<bool> debugOffloadLights{ false };
	std::atomic<bool> debugRTLights{ false };
	bool WantsLightPreview(DX12::SCMNativeMode mode) const;
	std::atomic<uint32_t> fullRTQuality{ DX12Native::kShadowQualityQuality };
	void SyncRuntimeSettings() noexcept;
	std::atomic<uint32_t> runtimeUnificationRadius{ DX12Native::kDefaultRuntimeUnificationRadius };
	std::atomic<uint32_t> preparedExclusionRadius{ DX12Native::kDefaultPreparedExclusionRadius };
	std::atomic<bool> additivePreparedEmission{ true };
	std::atomic<uint32_t> lightObservationFrames{ DX12Native::kDefaultLightObservationFrames };
	std::shared_ptr<const DX12Native::NativeLightPublication> CollectFullRTLights(uint32_t frame);
	bool ObserveFullRuntimeLights();
	DX12Native::NativeLightWorker lightWorker;
	std::shared_ptr<const DX12::Grid> fullRequestedGrid;
	std::shared_ptr<const std::vector<DX12Native::NativeRuntimeEmitter>> fullRuntimeSnapshot, fullRequestedRuntime;
	DX12Native::NativeLightSettings fullRequestedSettings;
	uint32_t fullRuntimeCell = 0, fullLastObservation = 0, fullLastRequestFrame = 0;
	bool fullObserved = false;
	void ResetFullLightInventory();
	struct RuntimeLightSample
	{
		const RE::NiLight* source = nullptr;  // Identity of the SCM light actually observed.
		uint32_t referenceID = 0;             // Corresponding prepared native-scene emitter.
		DX12::PreparedLight value;
	};
	mutable std::mutex runtimeLightMutex;
	uint32_t runtimeLightFrame = UINT32_MAX;
	std::vector<RuntimeLightSample> runtimeLightSamples;
	// Full-shadow membership is independent of SCM selection. Keep emitter identity alive
	// only while the source belongs to the active world light lists.
	struct FullRuntimeLight
	{
		RE::NiPointer<RE::NiLight> source;
		DX12::PreparedLight value;
		uint32_t placedReference = 0;
		uint64_t seen = 0;
		bool supported = false, sampled = false, active = false;
	};
	std::unordered_map<const RE::NiLight*, FullRuntimeLight> fullRuntimeLights;
	uint32_t fullRuntimeScope = 0;
	uint64_t fullLightVisit = 0;
};
