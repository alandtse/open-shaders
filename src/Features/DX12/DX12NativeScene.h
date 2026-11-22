#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "DX12NativeRuntimeCapture.h"
#include "DX12NativeShadowTrace.h"
#include "Feature.h"

#include <optional>
#include <string>

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
	bool WantsPreShadowMapsHook() const override { return true; }
	void PreShadowMaps() override;
	void LoadSettings(json& a_input) override;
	void SaveSettings(json& a_output) override;
	void RestoreDefaultSettings() override;
	void OnOpaqueRasterReady(const OpaqueRasterInputs& a_inputs) override;
	/** @brief Observe a stationary rigid caster during the existing SCM walk. */
	void ObserveSCMRigidCaster(RE::BSGeometry& geometry);
	void OnSceneTransitionReset(bool a_opening) override;
	void OnRuntimeDisabled() override;

	/** @brief Raw full-resolution shadow output, independently fenced for Native Composite. */
	std::shared_ptr<const DX12Native::ShadowMaskFrame> GetShadowMask(uint32_t frame) const { return shadows.Get(frame); }
	bool RetainShadowMask(const std::shared_ptr<const DX12Native::ShadowMaskFrame>& frame,
		ID3D12Fence* fence, uint64_t value) { return shadows.RetainThrough(frame, fence, value); }

	/** @brief Gets the early raster frame without waiting for GPU completion. */
	std::shared_ptr<const DX12Native::RasterFrame> GetRasterFrame(uint32_t a_frame) const { return raster.Get(a_frame); }
	/** @brief Keeps a borrowed raster frame alive until the consuming GPU work finishes. */
	bool RetainRasterFrame(const std::shared_ptr<const DX12Native::RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		return raster.RetainThrough(a_frame, a_fence, a_value);
	}

private:
	std::optional<GraphicsBackend> backend;
	DX12Native::RasterCapture raster;
	DX12Native::RuntimeGeometryCapture geometryCapture;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::ShadowMaskTrace shadows;
	bool shadowMaskEnabled = true;
	uint32_t densityMode = 0;
	std::string startupFailure;
};
