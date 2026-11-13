#pragma once

#include "Feature.h"
#include "NativeCompositePass.h"
#include <atomic>
#include <mutex>

/** @brief Owns the DX12 raster composite and optional native lighting contribution. */
struct NativeCompositeFeature final : Feature
{
	struct Settings
	{
		bool Enabled = true;
		bool AddLighting = true;
		float LightingStrength = 1.0f;
		bool DirectSpatialReconstruction = true;
		bool PersistLab = false;
		// Independent DX12 shadow layer. Does not require Native Lighting.
		bool ShadowLayerEnabled = true;
		float ShadowContribution = 2.0f;  // 0..4, boosts weighted occlusion beyond 1x.
		float ShadowBlend = 0.70f;        // 0..1, mixes shadow effect with unshadowed scene.
		float ShadowRadiusPixels = 2.0f, ShadowReceiverTolerance = 16.0f;
		bool ShadowPreview = false;
		bool ShadowAfterLighting = true;  // false: raster only; true: combined raster + native direct.
		bool ShadowSpatialReconstruction = true;
	};
	std::string GetName() override { return "Native Composite"; }
	std::string GetShortName() override { return "NativeComposite"; }
	bool IsCore() const override { return true; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	void DrawSettings() override;
	void LoadSettings(json& a_json) override;
	void SaveSettings(json& a_json) override;
	void RestoreDefaultSettings() override;
	void RegisterUxActions() override;
	/** @brief Legacy command bridge for scene-owned shadow layer settings. */
	void ConfigureShadowLayer(const json& args);
	json GetDiagnostics() override;
	/** @brief Produces a frame on the provided shared graphics backend. */
	std::shared_ptr<const NativeComposite::Frame> Capture(ID3D11Device5*, ID3D11DeviceContext4*, ID3D12Device*, ID3D12CommandQueue*,
		ID3D11Texture2D*, uint32_t, uint32_t, uint32_t, NativeComposite::Transfer);
	/** @brief Retains a published frame through a downstream GPU fence. */
	bool Retain(const std::shared_ptr<const NativeComposite::Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		return pass.Retain(a_frame, a_fence, a_value);
	}
	bool IsEnabled() const { return loaded && enabled.load(); }

private:
	NativeComposite pass;
	Settings settings;
	NativeComposite::Policy policy;
	uint64_t validityRevision = 1;
	std::mutex settingsMutex;
	std::atomic<bool> enabled{ true };
	std::atomic<uint64_t> captures{ 0 };
	std::atomic<uint64_t> lightingSubmissions{ 0 };
	std::atomic<uint64_t> shadowSubmissions{ 0 };
	std::atomic<uint32_t> lightingStatus{ 0 };
};
