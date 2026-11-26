#pragma once

#include "Buffer.h"
#include "NRD.h"
#include "NRDReblurIntegration.h"

#include <NRDSettings.h>

/**
 * ScreenSpaceReflections: hierarchical Hi-Z screen-space specular ray-march
 * with GGX importance sampling and ReBLUR denoising. Completely standalone —
 * runs whether or not Screen Space GI is enabled, and owns its full pipeline
 * (Hi-Z depth pyramid, specular ray march, NRD specular denoiser instance).
 *
 * Denoising guides (viewZ, packed normal+roughness, motion vectors) come from
 * the global NRD service; ScreenSpaceReflections only owns its own SSR-specific
 * intermediate textures (Hi-Z pyramid, raw and denoised radiance).
 */
struct ScreenSpaceReflections : Feature
{
	bool SupportsVR() override { return true; }
	virtual inline std::string GetName() override { return "Screen Space Reflections"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_reflections.name", "Screen Space Reflections"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceReflections"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		std::string desc = T(
			"feature.screen_space_reflections.description",
			"Hierarchical screen-space specular reflections. Uses a Hi-Z depth pyramid for fast ray marching and GGX importance sampling for physically based rough reflections, denoised by NVIDIA REBLUR.");

		return std::make_pair(
			desc,
			std::vector<std::string>{
				T("feature.screen_space_reflections.key_feature_1", "Hierarchical Hi-Z ray marching"),
				T("feature.screen_space_reflections.key_feature_2", "GGX importance sampling for rough reflections"),
				T("feature.screen_space_reflections.key_feature_3", "Dynamic cubemap fallback for off-screen rays"),
				T("feature.screen_space_reflections.key_feature_4", "NVIDIA REBLUR specular denoising") });
	}

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	void SetupNRDResources(uint32_t a_width, uint32_t a_height);
	/** @brief Whether the specular REBLUR denoiser should hold its pools this frame. */
	bool WantsReblur() const;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
	bool ShadersOK();
	bool CanDenoise() const;

	void DrawSSR();
	void UpdateSB();

	ID3D11ShaderResourceView* GetOutputTexture();

	//////////////////////////////////////////////////////////////////////////////////

	bool recompileFlag = false;
	bool tracedHalfRes = false;
	/// Set off the render thread when Enabled changes; DrawSSR rebuilds the deferred composite that bakes the SSR define.
	std::atomic<bool> queuedEnabledChange{ false };
	std::atomic<bool> queuedHistoryReset{ true };
	void QueueHistoryReset() { queuedHistoryReset.store(true, std::memory_order_release); }
	void DrawBufferViewer(float debugRescale) const;

	struct Settings
	{
		bool operator==(const Settings&) const = default;
		bool Enabled = true;
		bool HalfRes = true;

		float SpecularMult = 1.0f;
		float MaxTraceRoughness = 0.6f;
		uint SpecMaxSteps = 128;
		float SpecThickness = 5.0f;
		float NormalBias = 0.2f;
		float BRDFBias = 0.1f;
		float OcclusionStrength = 1.0f;

		bool UseDynamicCubemapsAsFallback = true;
		float SpecCubemapMult = 1.0f;

		float HitDistA = 210.0f;
		float HitDistB = 0.1f;
		float HitDistC = 20.0f;

		bool EnableREBLUR = true;
		NRD::REBLURSettings Reblur = {
			.MaxAccumulatedFrameNum = 20,
			.MaxFastAccumulatedFrameNum = 1,
			.MaxStabilizedFrameNum = 0,
			.FastHistoryClampingSigmaScale = 1.5f,
		};
		float SpecularPrepassBlurRadius = 50.0f;
		bool UsePrepassOnlyForSpecularMotionEstimation = true;

		float E11AmountScale = 1.0f;
	} settings;
	const void* GetSettingsBlob() const override { return &settings; }
	size_t GetSettingsBlobSize() const override { return sizeof(settings); }
	/** @brief The settings as the user left them; settings holds them with the Effects 11 overlay applied. */
	Settings userSettings;

	/** @brief The values an active Effects 11 preset drives: [EFFECT] EnableReflection and [REFLECTION]. */
	struct E11Overlay
	{
		bool active = false;
		bool enabled = false;
		float specularMult = 1.0f;
		bool operator==(const E11Overlay&) const = default;
	};
	E11Overlay appliedE11Overlay;

	/** @brief Whether an active Effects 11 preset drives SSR, which it does until the preset hands SSR back. */
	bool IsManagedByE11() const;
	/** @brief The overlay the active preset asks for now; inactive when Effects 11 does not drive SSR. */
	E11Overlay GetE11Overlay() const;
	/** @brief Reapplies the user's settings when the preset's overlay has changed since the last load. */
	void RefreshE11Overlay();

	struct alignas(16) SSRCB
	{
		float2 TexDim;
		float2 RcpTexDim;
		float2 FrameDim;
		float2 RcpFrameDim;
		uint FrameIndex;

		uint SpecMaxSteps;
		uint SpecMaxMips;
		float SpecThickness;

		float NormalBias;
		float BRDFBias;
		float OcclusionStrength;
		uint SpecUseDynamicCubemap;

		float SpecCubemapMult;
		float HitDistA;
		float HitDistB;
		float HitDistC;

		float MaxTraceRoughness;
		float pad0[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SSRCB);
	eastl::unique_ptr<ConstantBuffer> ssrCB;

	struct alignas(16) SharedData
	{
		uint Enabled;
		float SpecularMult;
		float SpecCubemapMult;
		uint pad0;
	};

	SharedData GetCommonBufferData();

	// Hi-Z depth pyramid (R32F, raw NDC depth with min-Z mip chain)
	eastl::unique_ptr<Texture2D> texHiZDepth = nullptr;
	static const uint kMaxHiZMips = 12;
	uint numHiZMips = 1;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kMaxHiZMips> hiZDepthSRVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, kMaxHiZMips> hiZDepthUAVs = { nullptr };

	// NRD specular textures (RGBA16F, full-res)
	eastl::unique_ptr<Texture2D> texNRDSpecInput = nullptr;
	eastl::unique_ptr<Texture2D> texNRDSpecOutput = nullptr;

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterHiZDepthCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterHiZMipsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> depthDownsampleCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> specularGICompute = nullptr;

	NRDReblurIntegration nrdReblurSpecular;
	nrd::ReblurSettings reblurSpecularSettings{};
	bool resetReblurHistory = true;
	bool reblurInitAttempted = false;
	uint lastDrawnFrame = 0;
	// True only after the current DrawSSR invocation has produced a trace output.
	bool outputReady = false;
	bool denoisedThisFrame = false;
};
