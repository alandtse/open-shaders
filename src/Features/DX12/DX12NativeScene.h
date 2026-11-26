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
	/** @brief Global prerequisites for suppressing SCM static shadows. */
	bool CanSuppressSCMStatic() const;
	// Snapshot LLF/SCM point-light parameters without altering the DX11 light.
	void ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID, bool flickers,
		const RE::NiPoint3& restOrigin);
	void OnSceneTransitionReset(bool a_opening) override;
	void OnRuntimeDisabled() override;

	/** @brief Raw mode-sized shadow output, independently fenced for Native Composite. */
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
	bool outdoorShadowOffloading = false;  // Off by default; vanilla raster shadows are unaffected.
	bool outdoorFullRT = false;
	uint32_t fullRTQuality = DX12Native::kShadowQualityQuality;
	std::vector<DX12::PreparedLight> CollectFullRTLights(const std::vector<DX12::PreparedLight>& prepared) const;
	struct RuntimeLightSample
	{
		const RE::NiLight* source = nullptr;  // Identity of the SCM light actually observed.
		uint32_t referenceID = 0;             // Corresponding prepared native-scene emitter.
		DX12::PreparedLight value;
	};
	mutable std::mutex runtimeLightMutex;
	uint32_t runtimeLightFrame = UINT32_MAX;
	std::vector<RuntimeLightSample> runtimeLightSamples;
};
