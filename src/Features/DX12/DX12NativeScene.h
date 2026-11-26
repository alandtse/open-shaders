#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "DX12NativeRuntimeCapture.h"
#include "DX12NativeShadowTrace.h"
#include "Feature.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/** @brief Independent early opaque raster capture for native DX12. */
struct DX12NativeScene final : Feature
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
	bool WantsRenderPassHook() const override;
	std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* pass) override;
	bool WantsPreShadowMapsHook() const override;
	void PreShadowMaps() override;
	/** @brief Starts asynchronous acceleration preparation for a menu-preloaded cell. */
	void PrepareMenuScene(std::shared_ptr<const DX12::Grid> grid);
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
	void ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID, bool flickers,
		const RE::NiPoint3& restOrigin);
	void OnSceneTransitionReset(bool a_opening) override;
	void OnRuntimeDisabled() override;

	/** @brief Raw full-resolution shadow output, independently fenced for Native Composite. */
	std::shared_ptr<const DX12Native::ShadowMaskFrame> GetShadowMask(uint32_t frame) const { return shadows.Get(frame); }

private:
	/** @brief Gates only native shadow replacement; never disables DX11 baseline shadows. */
	bool CanOffloadShadowsInCurrentCell() const;
	bool nativeSessionStarted = false;  // Render/main-thread lifecycle.
	std::optional<GraphicsBackend> backend;
	DX12Native::RasterCapture raster;
	DX12Native::RuntimeGeometryCapture geometryCapture;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::ShadowMaskTrace shadows;
	// One immutable, ordered light selection shared by SCM and the RayQuery dispatch.
	std::atomic<std::shared_ptr<const std::vector<DX12::PreparedLight>>> shadowLights;
	std::atomic<uint32_t> shadowLightScope{ 0 };
	std::atomic<std::shared_ptr<const DX12Native::TopLevel>> frameShadowScene;
	std::atomic<uint32_t> frameShadowSceneID{ UINT32_MAX };
	std::atomic<bool> previousShadowSubmitted{ false };
	std::atomic<bool> frameTraceAllLights{ false };
	bool shadowMaskEnabled = true;
	bool traceAllPreparedLights = false;
	bool outdoorShadowOffloading = false;  // Off by default; vanilla raster shadows are unaffected.
	bool offloadStaticShadows = true;
	bool offloadFlickeringStatic = true;
	static constexpr bool kDefaultFreezeNativeFlickering = true;
	bool freezeNativeFlickering = kDefaultFreezeNativeFlickering;
	uint32_t densityMode = DX12Native::kDefaultShadowDensityMode;
	bool nearLightShadowExclusion = true;
	uint32_t nearLightExclusionUnits = 16;
	float shadowReachMultiplier = DX12Native::kDefaultShadowReachMultiplier;  // Native only. Vanilla lights stay unchanged.
	float shadowFadeRange = DX12Native::kDefaultShadowFadeRange;
	float shadowFalloffCurve = 1.0f;
	bool shadowStochastic = true;
	float shadowEmitterRadiusMultiplier = DX12Native::kDefaultShadowEmitterRadiusMultiplier;
	struct RuntimeLightSample
	{
		const RE::NiLight* source = nullptr;  // Identity of the SCM light actually observed.
		uint32_t referenceID = 0;             // Corresponding prepared native-scene emitter.
		DX12::PreparedLight value;
		bool flickers = false;
	};
	mutable std::mutex runtimeLightMutex;
	uint32_t runtimeLightFrame = UINT32_MAX;
	std::vector<RuntimeLightSample> runtimeLightSamples;
	std::unordered_map<uint32_t, DX12::PreparedLight> frozenNativeLights;
};
