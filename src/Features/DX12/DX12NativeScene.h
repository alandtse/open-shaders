#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "DX12NativeRuntimeCapture.h"
#include "DX12NativeShadowTrace.h"
#include "FullShadowLightConsolidation.h"
#include "DX12ModeProfiles.h"
#include "../OverlayFeature.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

	void DrawSettings() override;
	/** @brief Developer-only native shadow tuning. */
	void DrawDevSettings();
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;
	/** @brief Cached frame-wide fast-path ownership for point-light static shadows. */
	bool AllLightStaticReady() const { return allLightStaticReady.load(std::memory_order_acquire); }
	bool WantsRenderPassHook() const override;
	std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* pass) override;
	bool WantsPreShadowMapsHook() const override;
	void PreShadowMaps() override;
	/** @brief Starts asynchronous acceleration preparation for a menu-preloaded cell. */
	void PrepareMenuScene(std::shared_ptr<const DX12::Grid> grid);
	/** @brief Retires a whole-cell build when it cannot fit the current DXGI budget. */
	bool RetireBudgetDeferredWholeCell(uint64_t revision)
	{
		if (!acceleration.WholeCellBudgetDeferred(revision))
			return false;
		acceleration.Reset();
		frameShadowScene.store(nullptr, std::memory_order_release);
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
	bool IsPreparedStaticCaster(RE::BSGeometry& geometry) const;
	bool TraceAllPreparedLights() const { return frameTraceAllLights.load(std::memory_order_acquire); }
	std::shared_ptr<const DX12Native::TopLevel> GetFrameShadowScene() const;
	/** @brief Global prerequisites for suppressing SCM static shadows. */
	bool CanSuppressSCMStatic() const;
	// Snapshot LLF/SCM point-light parameters without altering the DX11 light.
	void ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID, const RE::NiPoint3& restOrigin);
	void OnSceneTransitionReset(bool a_opening) override;
	void OnRuntimeDisabled() override;

	/** @brief Raw quality-sized shadow output, independently fenced for Native Composite. */
	std::shared_ptr<const DX12Native::ShadowMaskFrame> GetShadowMask(uint32_t frame) const { return shadows.Get(frame); }

private:
	/** @brief Gates only native shadow replacement; never disables DX11 baseline shadows. */
	bool CanOffloadShadowsInCurrentCell() const;
	std::shared_ptr<const std::vector<DX12::PreparedLight>> CollectFullShadowLights(const std::shared_ptr<const std::vector<DX12::PreparedLight>>& publication);
	void ResetFullLightInventory();
	bool nativeSessionStarted = false;  // Render/main-thread lifecycle.
	std::optional<GraphicsBackend> backend;
	DX12Native::RasterCapture raster;
	DX12Native::RuntimeGeometryCapture geometryCapture;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::ShadowMaskTrace shadows;
	// One immutable, ordered light selection shared by SCM and the RayQuery dispatch.
	struct PreparedLightSelection
	{
		std::shared_ptr<const std::vector<DX12::PreparedLight>> ordered;
		std::unordered_map<uint32_t, size_t> byReference;
	};
	std::atomic<std::shared_ptr<const PreparedLightSelection>> shadowLights;
	std::atomic<uint32_t> shadowLightScope{ 0 };
	std::atomic<std::shared_ptr<const DX12Native::TopLevel>> frameShadowScene;
	std::atomic<uint32_t> frameShadowSceneID{ UINT32_MAX };
	std::atomic<bool> previousShadowSubmitted{ false };
	std::atomic<bool> frameTraceAllLights{ false };
	std::atomic<bool> allLightStaticReady{ false };
	struct ShadowProfile
	{
		explicit ShadowProfile(uint32_t quality, float reach) : qualityMode(quality), shadowReachMultiplier(reach) {}
		std::atomic<bool> debugNativeLights{ false };
		std::atomic<bool> additivePreparedEmission{ true };
		std::atomic<uint32_t> runtimeUnificationRadius{ DX12Native::kDefaultRuntimeUnificationRadius };
		std::atomic<uint32_t> preparedExclusionRadius{ DX12Native::kDefaultPreparedExclusionRadius };
		bool outdoorShadowOffloading = false;  // Off by default; vanilla raster shadows are unaffected.
		uint32_t qualityMode;
		bool nearLightShadowExclusion = true;
		uint32_t nearLightExclusionUnits = 16;
		float shadowReachMultiplier;  // Native only. Vanilla lights stay unchanged.
		float shadowFadeRange = DX12Native::kDefaultShadowFadeRange;
		float shadowFalloffCurve = 1.0f;
	};
	struct OffloadingProfile
	{
		std::atomic<bool> debugNativeLights{ false };
		bool outdoorShadowOffloading = false;
		bool performance = true;
	};
	static constexpr uint32_t kRayTracedDefaultQuality = DX12Native::kShadowQualityMedium;
	static constexpr float kRayTracedDefaultReach = DX12Native::kDefaultShadowReachMultiplier;
	OffloadingProfile offloadingProfile;
	ShadowProfile rayTracedProfile{ kRayTracedDefaultQuality, kRayTracedDefaultReach };
	void LoadOffloadingProfile(const json& input);
	json SaveOffloadingProfile() const;
	static void LoadProfile(ShadowProfile& profile, const json& input, uint32_t defaultQuality, float defaultReach);
	static json SaveProfile(const ShadowProfile& profile);
	struct RuntimeLightSample
	{
		const RE::NiLight* source = nullptr;  // Identity of the SCM light actually observed.
		uint32_t referenceID = 0;             // Corresponding prepared native-scene emitter.
		DX12::PreparedLight value;
	};
	mutable std::mutex runtimeLightMutex;
	uint32_t runtimeLightFrame = UINT32_MAX;
	std::vector<RuntimeLightSample> runtimeLightSamples;
	std::unordered_map<const RE::NiLight*, size_t> runtimeLightBySource;
	std::unordered_map<uint32_t, size_t> runtimeLightByReference;
	void ClearRuntimeLightSamples();
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
	std::shared_ptr<const std::vector<DX12::PreparedLight>> fullPreparedSnapshot;
	std::shared_ptr<const std::vector<DX12::PreparedLight>> fullConsolidatedLights;
	uint32_t fullCachedUnificationRadius = 0, fullCachedExclusionRadius = 0;
	bool fullCachedAdditiveEmission = false;
	uint32_t fullRuntimeScope = 0;
	uint64_t fullLightVisit = 0;
};
