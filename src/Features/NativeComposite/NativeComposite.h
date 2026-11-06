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
		bool PersistLab = false;
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
	json GetDiagnostics() override;
	/** @brief Produces a frame on the provided shared graphics backend. */
	std::shared_ptr<const NativeComposite::Frame> Capture(ID3D11Device5*, ID3D11DeviceContext4*, ID3D12Device*, ID3D12CommandQueue*,
		ID3D11Texture2D*, uint32_t, uint32_t, uint32_t, NativeComposite::Transfer);
	/** @brief Retains a published frame through a downstream GPU fence. */
	bool Retain(const std::shared_ptr<const NativeComposite::Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{ return pass.Retain(a_frame, a_fence, a_value); }
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
	std::atomic<uint32_t> lightingStatus{ 0 };
};
