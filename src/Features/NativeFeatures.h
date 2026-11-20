#pragma once

#include "Feature.h"
#include "Features/DX12NativeScene/PreparedScene.h"
#include "LightLimitFix.h"
#include "Features/NativePBR/MaterialRouting.h"
#include "Features/NativePBR/MaterialDiscovery.h"
#include "Features/NativePBR/NativePBRRegistry.h"
#include "Features/NativePBR/MaterialCatalog.h"
#include "Utils/BootSnapshot.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <unordered_set>
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
	struct RayMaterialBindings;
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
		// Restore Extended Materials only for the remaining DX11 raster passes.
		// This never re-enables legacy TruePBR or transfers raster material authority to DX12.
		bool LegacyCompatibilityMaterials = false;
	} backendSettings;
	inline static constexpr Util::Settings::RestartTable<BackendSettings, 2> kRestartFields{ {
		UTIL_RESTART_FIELD(BackendSettings, NativeBackend, "PBR Backend"),
		UTIL_RESTART_FIELD(BackendSettings, LegacyCompatibilityMaterials, "Legacy Compatibility Materials"),
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
	NativePBR::MaterialDiscovery materialDiscovery;
	NativePBR::Registry& materialRegistry = materialDiscovery.MutableRegistry();
	NativePBR::MaterialCatalog materialCatalog;
	std::shared_ptr<const DX12Native::TopLevel> discoveredGeometry;
	std::unordered_set<std::string> discoveredMaterialKeys;
	void DiscoverPreparedAssets();
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
	/** @brief Returns generation-matched material records indexed by TLAS instance ID. */
	std::shared_ptr<const NativePBR::RayMaterialBindings> GetRayMaterialBindings() const;
	std::atomic<std::shared_ptr<const NativePBR::RayMaterialBindings>> rayMaterialBindings;
	std::atomic<bool> enabled{ true };
	std::atomic<bool> nativeParallax{ false };
	std::atomic<bool> nativeComplexMaterials{ false };
	std::atomic<bool> forceRayAdmission{ false };
	struct SnowSettings
	{
		bool coverEligible = false, ultravioletAffected = true, followParallaxUV = false;
		float noiseAmount = 1.0f, noiseRaysPerPatch = 4.0f;
		float sparkleAmount = 1.0f, sparkleDensity = 0.25f, roughness = 1.0f;
		float ultravioletIntensity = 0.006f, colorAffection = 0.65f, specularAmount = 0.5f;
		float lumpHeight = 1.5f, lumpSize = 8.0f, lumpDensity = 1.0f, flatness = 1.0f;
		float edgeWaviness = 0.2f;
		uint32_t lumpLayers = 4;
	};
	std::atomic<bool> snowCoverEligible{ false };
	/** @brief Provides the locked native snow preset and session-only interior test state. */
	SnowSettings GetSnowSettings() const
	{
		SnowSettings settings{};
		settings.coverEligible = snowCoverEligible.load(std::memory_order_relaxed);
		return settings;
	}
	// 1 geometry, 2 evaluated NativePBR, 3 accepted Native Lighting. Early stages are deliberate diagnostics.
	std::atomic<uint32_t> readinessStage{ 3 };
	uint32_t NativeFirstReadinessStage() const { return IsActive() ? std::clamp(readinessStage.load(std::memory_order_relaxed), 1u, 3u) : 0u; }
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
		static constexpr std::array<std::string_view, 1> kTruePbrOnly{ "TruePBR" };
		const bool compatibility = backendLatched.load(std::memory_order_acquire) ?
			bootSnapshot.Boot(&BackendSettings::LegacyCompatibilityMaterials) : backendSettings.LegacyCompatibilityMaterials;
		return SelectedBackend() == NativePBR::Backend::NativeDX12 ?
			(compatibility ?
				std::span<const std::string_view>(kTruePbrOnly) : std::span<const std::string_view>(kLegacyFeatures)) :
			std::span<const std::string_view>{};
	}
	bool IsActive() const;
	void DrawSettings() override;
	void LoadSettings(json&) override;
	void SaveSettings(json&) override;
	void RegisterUxActions() override;
	void RestoreDefaultSettings() override
	{
		backendSettings = {}; enabled = true; nativeParallax = false; nativeComplexMaterials = false;
		forceRayAdmission = false; readinessStage = 3;
		snowCoverEligible = false;
	}
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
		uint32_t falloffMode = 0;
	};
	struct LightFrame
	{
		uint32_t frame = 0;
		std::vector<Light> lights;
	};
	std::atomic<bool> enabled{ true };
	// Prepared light positions and identities stay authoritative; LLF radiance
	// can be added experimentally without changing SCM's source identities.
	std::atomic<bool> contributeUnmatched{ true };
	std::atomic<bool> contributeMatched{ false };
	struct LightControls
	{
		bool inheritOriginal = true, useVanillaMatches = false;
		float sourceSize = 1.0f, intensity = 1.0f, reach = 1.0f;
		bool operator==(const LightControls&) const = default;
	};
	std::atomic<bool> inheritOriginal{ true }, useVanillaMatches{ false };
	std::atomic<float> customSourceSize{ 1.0f }, customIntensity{ 1.0f }, customReach{ 1.0f };

	std::atomic<float> matchDistanceUnits{ 64.0f };
	// Radiance gain for opted-in LLF contributions.
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
		uint32_t count = 0, prepared = 0, observed = 0, matched = 0, matchedContributed = 0, added = 0;
		uint32_t clusteredLive = 0, omitted = 0, overflow = 0;
	};
	UnifiedLights BuildUnifiedLights(std::span<const DX12Native::PreparedPointLight> prepared,
		uint32_t frame, bool allowRecent = false) const;
	void PublishLights(const LightLimitFix::LightData* data, uint32_t count, const float3& eye, uint32_t frame);
	/** Clear the native light snapshot when a cell changes or a loading screen opens/closes. */
	void ClearCapturedLights();
	std::shared_ptr<const LightFrame> GetLightFrame(uint32_t frame) const;
	std::shared_ptr<const LightFrame> GetRecentLightFrame(uint32_t frame) const;
private:
	struct UnifiedWork
	{
		std::atomic<bool> done{ false };
		std::shared_ptr<const LightFrame> source;
		std::shared_ptr<const DX12Native::PreparedCell> prepared;
		float matchUnits = 0.0f, gain = 0.0f;
		bool contribute = false, contributeMatches = false, valid = false;
		LightControls controls;
		UnifiedLights result;
	};
	mutable std::mutex lightMutex;
	std::shared_ptr<const LightFrame> lightFrame;
	// The native worker owns immutable copies; neither engine pointers nor
	// prepared-scene memory may be freed while it evaluates a generation.
	mutable std::atomic<std::shared_ptr<const UnifiedWork>> workerUnified;
	mutable std::atomic<std::shared_ptr<const UnifiedWork>> fallbackUnified;
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
	void RegisterUxActions() override;
	void OnSceneTransitionReset(bool a_open) override;
	void OnRuntimeDisabled() override { ClearCapturedLights(); }
	void GameLoaded() override { ClearCapturedLights(); }
	void RestoreDefaultSettings() override
	{
		enabled = true;
		contributeUnmatched = true;
		contributeMatched = false;
		inheritOriginal = true;
		useVanillaMatches = false;
		customSourceSize = 1.0f;
		customIntensity = 1.0f;
		customReach = 1.0f;
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
