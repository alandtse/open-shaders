#pragma once

#include "Feature.h"
#include "Features/DX12NativeScene/PreparedScene.h"
#include "Features/DX12NativeScene/RasterCapture.h"
#include "Features/DX12NativeScene/ShadowTrace.h"
#include <atomic>
#include <optional>
#include <mutex>

/** @brief Loads prepared geometry and publishes its completed DX12 acceleration scene. */
struct DX12NativeScene final : Feature
{
	struct Settings { bool Enabled = false; bool ShadowMask = false; uint32_t BudgetMB = 512; } settings;
	struct PublishedScene
	{
		std::shared_ptr<const DX12Native::PreparedCell> cell;
		std::shared_ptr<const DX12Native::TopLevel> acceleration;
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
	void OnRuntimeDisabled() override;
	void OnSceneTransitionReset(bool a_open) override;
	void GameLoaded() override;
	void ClearShaderCache() override { reloadRequested = true; }
	json GetDiagnostics() override;
	void RegisterUxActions() override;
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
	void DispatchShadow(const char* a_stage);
	std::optional<GraphicsBackend> backend;
	DX12Native::PreparedScene prepared;
	DX12Native::RasterCapture raster;
	DX12Native::ShadowTrace shadow;
	DX12Native::AccelerationWorker acceleration;
	std::string failure, shadowStatus, shadowStage;
	uint64_t revision = 0;
	uint32_t cellID = 0;
	std::atomic<bool> resetRequested{ false }, reloadRequested{ false };
	std::atomic<int> requestedEnabled{ -1 }, requestedShadow{ -1 };
	std::atomic<uint32_t> requestedBudget{ 0 };
	mutable std::mutex settingsMutex, diagnosticsMutex;
	json diagnostics = json::object();
	bool submitted = false;
	bool transitionOpen = false;
};
