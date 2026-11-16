#pragma once

#include "Feature.h"
#include "Features/DX12NativeScene/PreparedScene.h"
#include "LightLimitFix.h"
#include "Features/NativePBR/MaterialRouting.h"
#include "Features/NativePBR/MaterialSidecar.h"
#include "Features/NativePBR/NativePBRRegistry.h"
#include "Features/NativePBR/MaterialCatalog.h"
#include "Utils/BootSnapshot.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>
namespace RE
{
	class BSShadowLight;
}
namespace NativePBR
{
	struct RecipeGeneration;
	struct MaterialPublication;
	struct GpuMaterialResources;
	struct PreparedSceneMaterials;
struct SurfaceFrame;
class SurfaceFramePublisher;
}

/** Additional DX12 features. The original TruePBR/LLF/SCM implementations are
 * unchanged as the legacy baseline and continue to collect Skyrim state.
 * Each native feature takes effect when the DX12 Native Scene backend is
 * selected. Missing outputs do not trigger per-effect legacy fallback.
 */
struct NativePBRFeature final : Feature
{
	NativePBRFeature();
	~NativePBRFeature();
	struct BackendSettings
	{
		bool NativeBackend = true;
	} backendSettings;
	inline static constexpr Util::Settings::RestartTable<BackendSettings, 1> kRestartFields{ {
		UTIL_RESTART_FIELD(BackendSettings, NativeBackend, "PBR Backend"),
	} };
	Util::Settings::BootSnapshot<BackendSettings> bootSnapshot{ kRestartFields };
	std::once_flag backendLatchOnce;
	std::atomic<bool> backendLatched{ false };
	std::atomic<NativePBR::Backend> bootBackend{ NativePBR::Backend::LegacyDX11 };
	struct MaterialSummary
	{
		size_t recipes = 0, pending = 0, incomplete = 0, observedTextures = 0, sharedCandidates = 0;
		std::array<size_t, 13> issues{};
		bool failed = false;
	};
	struct MaterialWorker;
	// NativePBR owns identities, revisions, immutable snapshots and recipe generations.
	// TruePBR only produces observations from Skyrim's existing hooks.
	NativePBR::Registry materialRegistry;
	NativePBR::MaterialCatalog materialCatalog;
	std::shared_ptr<MaterialWorker> materialWorker;
	void PollMaterialRegistry();
	void Prepass() override;
	void OnSceneTransitionReset(bool) override;
	void SetupResources() override;
	MaterialSummary GetMaterialSummary() const;
	std::shared_ptr<const NativePBR::RecipeGeneration> GetNativeRecipes() const;
	std::shared_ptr<const NativePBR::MaterialPublication> GetMaterialPublication() const;
	std::shared_ptr<const NativePBR::GpuMaterialResources> GetNativeGpuMaterials() const;
	/** @brief Returns scene-owned geometry and textures with identity-only material attachments. */
	std::shared_ptr<const NativePBR::PreparedSceneMaterials> GetPreparedSceneMaterials() const;
	std::atomic<std::shared_ptr<const NativePBR::PreparedSceneMaterials>> preparedSceneMaterials;
	std::atomic<bool> enabled{ true };
	std::atomic<bool> previewOpaqueReplacement{ false };
	std::atomic<bool> captureProjectedUv{ false };
	std::atomic<bool> experimentalInputOnly{ false };
	// 0 = material identity; 1 = UV/derivatives; 2 = PBR coverage comparison.
	std::atomic<uint32_t> previewVisualization{ 0 };
	NativePBR::MaterialSidecar materialSidecar;
	std::unique_ptr<NativePBR::SurfaceFramePublisher> surfacePublisher;
	winrt::com_ptr<ID3D11Device5> surfaceDevice11;
	winrt::com_ptr<ID3D11DeviceContext4> surfaceContext11;
	winrt::com_ptr<ID3D12Device> surfaceDevice12;
	winrt::com_ptr<ID3D12CommandQueue> surfaceQueue12;
	std::string surfaceHandoffStatus = "Awaiting opaque raster completion";
	uint64_t surfaceHandoffCalls = 0;
	void PrepareSidecar(ID3D11Device* a_device);
	void BeginOpaqueSidecar();
	void BeginSidecarPass(uint64_t a_materialIdentity, uint64_t a_materialRevision = 0, std::string a_referenceKey = {},
		std::string a_shapeName = {}, NativePBR::PreviewSurveyClass a_surveyClass = NativePBR::PreviewSurveyClass::None,
		uint32_t a_unsupportedReasons = 0);
	void EndSidecarPass();
	bool TryInputOnlyPass(const RE::BSShader& a_shader, uint32_t a_descriptor);
	bool BeginInputOnlyDraw(ID3D11DeviceContext* a_context);
	void EndInputOnlyDraw();
	void ReplaySidecarDraw(ID3D11DeviceContext* a_context, UINT a_count, UINT a_first, INT a_base, bool a_inputBound);
	void FinishOpaqueSidecar(ID3D11ShaderResourceView* a_finalDepth);
	/** @brief Registers the DX12 presentation backend without depending on its owning feature. */
	void BindSurfaceInterop(ID3D11Device5*, ID3D11DeviceContext4*, ID3D12Device*, ID3D12CommandQueue*);
	/** @brief Publishes this frame's opaque material attachments after DX11 raster completion. */
	void OnOpaqueRasterReady(const OpaqueRasterInputs&) override;
	/** @brief Returns the independently published material frame, if the IDs match. */
	std::shared_ptr<const NativePBR::SurfaceFrame> GetSurfaceFrame(uint32_t a_frame) const;
	NativePBR::MaterialSidecar::Sources GetSidecarSources(uint32_t a_frame) const;
	std::string GetName() override { return "NativePBR"; }
	std::string GetShortName() override { return "NativePBR"; }
	std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }
	bool IsCore() const override { return true; }
	std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override
	{
		return { kRestartFields.data(), kRestartFields.size() };
	}
	const void* GetBootValue(std::string_view a_key) const override { return bootSnapshot.RawBoot(a_key); }
	const void* GetSettingsBlob() const override { return &backendSettings; }
	size_t GetSettingsBlobSize() const override { return sizeof(backendSettings); }
	/** @brief Captures the selected rendering backend exactly once for this session. */
	void LatchBackendAtBoot();
	void PostPostLoad() override;
	/** @brief Selects the PBR backend independently of DX12 device readiness. */
	NativePBR::Backend SelectedBackend() const;
	std::span<const std::string_view> GetBootSuppressedFeatures() const override
	{
		static constexpr std::array<std::string_view, 2> kLegacyFeatures{ "TruePBR", "ExtendedMaterials" };
		return SelectedBackend() == NativePBR::Backend::NativeDX12 ?
			std::span<const std::string_view>(kLegacyFeatures) : std::span<const std::string_view>{};
	}
	bool IsActive() const;
	/** @brief True when eligible FXC input permutations are staged for raster-safe native capture. */
	bool OwnsInputOnlyShading() const;
	void DrawSettings() override;
	void LoadSettings(json&) override;
	void SaveSettings(json&) override;
	void RestoreDefaultSettings() override { backendSettings = {}; enabled = true; previewOpaqueReplacement = false; captureProjectedUv = false; experimentalInputOnly = false; previewVisualization = 0; }
	json GetDiagnostics() override;
};

struct NativeLLFFeature final : Feature
{
	// Native light generations belong to NativeLLF, never to the DX11 LLF baseline.
	// The baseline exposes one post-override CPU buffer at its existing update boundary.
	struct Light
	{
		float3 position{}, radiance{};
		float radius = 0.0f, sourceRadius = 0.0f, fadeZone = 0.0f;
		uint32_t flags = 0, shadowSlot = 0;
	};
	struct LightFrame
	{
		uint32_t frame = 0;
		std::vector<Light> lights;
	};
	std::atomic<bool> enabled{ true };
	// Only supplementary LLF emitters are toggled; prepared scene lighting
	// stays authoritative, while live observations remain available to SCM.
	std::atomic<bool> contributeUnmatched{ true };
	std::atomic<bool> bakeUnmatchedIntoScene{ true };  // Runtime scene layer; not an offline file.

	std::atomic<float> matchDistanceUnits{ 64.0f };
	// Radiance gain for supplementary LLF-only emitters, never prepared scene lights.
	std::atomic<float> contribution{ 1.0f };
	static constexpr uint32_t kMaxUnifiedLights = DX12Native::kMaximumNativePointLights;
	struct SourceIdentity
	{
		float3 position{};
		float radius = 0.0f;
		uint32_t flags = 0;
		uint32_t shadowSlot = UINT32_MAX;
		bool valid = false;
	};
	struct UnifiedLights
	{
		std::array<DX12Native::PreparedPointLight, kMaxUnifiedLights> lights{};
		std::array<SourceIdentity, kMaxUnifiedLights> source{};
		uint32_t count = 0, prepared = 0, observed = 0, matched = 0, added = 0;
		uint32_t clusteredLive = 0, omitted = 0, overflow = 0;
	};
	UnifiedLights BuildUnifiedLights(std::span<const DX12Native::PreparedPointLight> prepared,
		uint32_t frame, bool allowRecent = false) const;
	void PublishLights(const LightLimitFix::LightData* data, uint32_t count, const float3& eye, uint32_t frame);
	/** Clear the native light snapshot when a cell changes or a loading screen opens/closes. */
	void ClearCapturedLights();
	std::shared_ptr<const LightFrame> GetLightFrame(uint32_t frame) const;
	std::shared_ptr<const LightFrame> GetRecentLightFrame(uint32_t frame) const;
	/** Publish a completed scene light generation without waiting for workers. */
	std::shared_ptr<const DX12Native::NativeSceneLightBake> GetSceneLightBake(
		const std::shared_ptr<const DX12Native::PreparedCell>& baseline) const;

private:
	struct UnifiedWork
	{
		std::atomic<bool> done{ false };
		std::shared_ptr<const LightFrame> source;
		std::shared_ptr<const DX12Native::PreparedCell> prepared;
		std::shared_ptr<const DX12Native::NativeSceneLightBake> previousBake, baked;
		uint64_t bakeEpoch = 0;
		float matchUnits = 0.0f, gain = 0.0f;
		bool contribute = false, valid = false;
		UnifiedLights result;
	};
	mutable std::mutex lightMutex;
	std::shared_ptr<const LightFrame> lightFrame;
	// The native worker owns immutable copies; neither engine pointers nor
	// prepared-scene memory may be freed while it evaluates a generation.
	mutable std::atomic<std::shared_ptr<const UnifiedWork>> workerUnified;
	mutable std::atomic<std::shared_ptr<const UnifiedWork>> fallbackUnified;
	mutable std::atomic<std::shared_ptr<const DX12Native::NativeSceneLightBake>> sceneBake;
	std::atomic<uint64_t> bakeEpoch{ 1 };
	mutable std::atomic<uint64_t> bakedPublications{ 0 };
	mutable std::atomic<uint64_t> workerHits{ 0 }, workerQueued{ 0 }, fallbackHits{ 0 };
	std::atomic<bool> transitionOpen{ false };

public:
	std::string GetName() override { return "NativeLLF"; }
	std::string GetShortName() override { return "NativeLLF"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool IsActive() const;
	void DrawSettings() override;
	void LoadSettings(json&) override;
	void SaveSettings(json&) override;
	void OnSceneTransitionReset(bool a_open) override;
	void OnRuntimeDisabled() override { ClearCapturedLights(); }
	void GameLoaded() override { ClearCapturedLights(); }
	void RestoreDefaultSettings() override
	{
		enabled = true;
		contributeUnmatched = true;
		bakeUnmatchedIntoScene = true;
		matchDistanceUnits = 64.0f;
		contribution = 1.0f;
	}
	json GetDiagnostics() override;
};

struct NativeSCMFeature final : Feature
{
	std::atomic<bool> enabled{ true };
	/** Exact LLF/SCM slot identity selected for native ray-shadow evaluation.
     *  This is a per-light ownership decision, never a renderer fallback gate. */
	bool OwnsStaticForLight(const RE::BSShadowLight* light, int32_t slot) const;
	std::string GetName() override { return "NativeSCM"; }
	std::string GetShortName() override { return "NativeSCM"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool IsActive() const;
	void DrawSettings() override;
	void LoadSettings(json&) override;
	void SaveSettings(json&) override;
	void RestoreDefaultSettings() override { enabled = true; }
	json GetDiagnostics() override;
};
