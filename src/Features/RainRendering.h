#pragma once

#include "Buffer.h"
#include "Feature.h"

#include <array>
#include <memory>
#include <optional>
#include <winrt/base.h>

/** @brief GPU-driven, world-space airborne rain rendering. */
struct RainRendering : Feature
{
	/** @brief Runtime-tunable airborne rain controls. */
	struct Settings
	{
		uint EnableRainRendering = 1;
		uint ForceRainRendering = 0;
		uint EnableRainRoofOcclusion = 1;
		uint EnableRainCanopyResponse = 1;
		uint RainDropCount = 17352;
		uint RainOverheadDropCount = 8;
		float RainDensity = 1.0f;
		float RainFallSpeed = 2600.0f;

		float RainStreakLength = 72.0f;
		float RainVelocityStretch = 0.045f;
		float RainStreakWidth = 5.18f;

		float RainOpacity = 0.52f;
		float RainBrightness = 0.85f;
		float RainLightingResponse = 0.50f;
		float RainMinimumVisibility = 0.01f;
		float RainNearCutoffDistance = 4.0f;
		float RainFarDistance = 6000.0f;
		float RainNearLayerDistance = 422.0f;
		float RainMidLayerDistance = 2371.0f;
		float RainNearBudgetWeight = 1.02f;
		float RainMidBudgetWeight = 1.11f;
		float RainFarBudgetWeight = 0.25f;
		uint EnableDistantRain = 1;
		uint RainDistantDropCount = 4096;
		float RainDistantDensity = 1.0f;
		float RainDistantOpacity = 0.55f;
		float RainDistantStreakLength = 64.0f;
		float RainDistantStreakWidth = 3.0f;
		float RainDensityNoiseScale = 3200.0f;
		float RainDensityNoiseStrength = 0.65f;

		float RainCurtainScale = 7500.0f;
		float RainCurtainStrength = 0.80f;
		float RainCurtainContrast = 1.75f;

		float RainCurtainMinDensity = 0.28f;
		float RainCurtainMaxDensity = 1.85f;

		float RainIntersectionFadeDistance = 96.0f;
		uint RainDebugMode = 0;

		uint EnableGlassyRain = 1;
		uint EnableRainRefraction = 1;
		float RainCoreDarkening = 0.08f;
		float RainEdgeHighlight = 1.0f;
		float RainRefractionStrength = 2.68f;
		float RainRefractionDistance = 4800.0f;
		float RainStreakVariation = 0.45f;
		float RainLocalLightResponse = 0.6f;
		uint EnableTexturedRain = 1;
		float RainTextureNormalStrength = 2.0f;
		float RainTextureReflectionStrength = 1.0f;
		float RainTextureUVWidth = 0.5f;
		float RainEnvironmentTransmission = 0.8f;
		float RainSceneRefractionMix = 1.0f;
		float RainHighlightRoughness = 0.18f;
		float RainLightScattering = 0.25f;
		float RainRoofOcclusionFadeStart = 0.20f;
		float RainRoofOcclusionFadeEnd = 0.75f;
		float RainCanopyDensityScale = 0.35f;
		float RainCanopySpeedScale = 0.85f;
	};

	/** @brief Per-draw constants mirrored by RainRendering.hlsl. */
	struct alignas(16) PerFrame
	{
		float4 HeadPositionAndTime;
		float4 VolumeSizeAndDensity;
		float4 WeatherFallDepth;
		float4 Streak;
		float4 Appearance;
		float4 DistanceNoise;
		float4 Curtain;
		float4 CurtainDensity;
		float4 LightColor;
		float4 CameraData;
		std::array<uint32_t, 4> GridAndDebug;
		float4 Glassy;
		float4 Refraction;
		float4 ScreenSize;
		float4 LocalLighting;
		std::array<uint32_t, 4> LightGrid;
		float4 TexturedRain;
		float4 RainTextureShape;
		float4 LayerRadii;
		std::array<uint32_t, 4> LayerCounts;
		float4 MaterialLighting;
		float4 RoofOcclusion;
		float4 DistantRain;
		float4 Canopy;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	static_assert(sizeof(PerFrame) == 384, "RainRendering::PerFrame must match the rain shaders");

	/** @brief GPU render record produced once per drop and consumed by both eyes. */
	struct alignas(16) DropData
	{
		float4 PositionLength;
		float4 VelocityWidth;
		float4 ColorOpacity;
		float4 LightDirection;
	};
	STATIC_ASSERT_ALIGNAS_16(DropData);
	static_assert(sizeof(DropData) == 64, "RainRendering::DropData must match RainRendering.hlsl");

	Settings settings;

	std::string GetName() override { return "Rain Rendering"; }
	std::string GetShortName() override { return "RainRendering"; }
	std::string GetDisplayName() override { return "Airborne Rain"; }
	std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	bool SupportsVR() override { return true; }

	/** @brief Returns the feature description and principal visual guarantees. */
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	/** @brief Creates the constant buffer and fixed-function render states. */
	void SetupResources() override;
	/** @brief Draws airborne rain controls. */
	void DrawSettings() override;
	/** @brief Draws the rain density and range controls in the VR performance panel. */
	void DrawPerformanceSettings() override;
	std::string GetPerformanceSectionLabel() override { return GetDisplayName(); }
	int GetPerformanceOrder() const override { return 45; }
	/** @brief Applies the selected rain GPU-budget tier. */
	void ApplyPerformanceProfile(PerfProfile a_profile) override;
	/** @brief Returns whether the rain GPU budget matches the selected tier. */
	bool MatchesPerformanceProfile(PerfProfile a_profile) const override;
	/** @brief Describes the rain GPU budget selected by a performance tier. */
	std::string GetProfilePreviewText(PerfProfile a_profile) const override;
	/** @brief Loads persisted rain settings. */
	void LoadSettings(json& o_json) override;
	/** @brief Saves persisted rain settings. */
	void SaveSettings(json& o_json) override;
	/** @brief Restores the feature defaults. */
	void RestoreDefaultSettings() override;
	/** @brief Releases runtime-compiled shaders so they can be rebuilt on demand. */
	void ClearShaderCache() override;

	/** @brief Draws rain before water when no water composite was observed recently. */
	void DrawBeforeWater();
	/** @brief Draws rain after the water composite and records the water-active frame. */
	void DrawAfterWater();
	/** @brief Returns whether this feature replaces the supplied weather's vanilla rain particles. */
	bool ReplacesVanillaRain(const RE::TESWeather* a_weather) const;
	/** @brief Returns the rain-owned depth target used for the solid-cover occlusion pass. */
	Texture2D* GetSolidCoverOcclusionTarget() const
	{
		const bool rainActive = settings.ForceRainRendering || GetWeatherIntensity() > 0.0f;
		return settings.EnableRainRoofOcclusion && settings.EnableRainCanopyResponse && rainActive &&
		               canopyOcclusionCS ?
		           solidCoverOcclusion.get() :
		           nullptr;
	}

private:
	struct WeatherRainState
	{
		float intensity = 0.0f;
		float fallSpeedScale = 1.0f;
	};

	void DrawRain();
	void ApplyGlassyReferenceSettings();
	void NormalizeSettings();
	std::array<uint32_t, 4> GetLayerDropCounts() const;
	float4 GetLayerRadii(float a_farDistance) const;
	WeatherRainState GetWeatherRainState() const;
	float GetWeatherIntensity() const;
	float3 GetRainLightColor() const;
	bool EnsureShaders();
	bool EnsureDistantRainShaders();
	bool EnsureRainSampler();
	bool EnsureRainTexture();
	bool EnsureSceneColorShaders();
	bool EnsureCanopyOcclusionResources();
	bool EnsureCanopyOcclusionShader();
	void ResetCanopyOcclusion();
	void UpdateCanopyOcclusion(ID3D11DeviceContext* a_context, ID3D11Buffer* a_sharedBuffer, ID3D11Buffer* a_frameBuffer);
	ID3D11ShaderResourceView* GetRainEnvironment() const;
	bool EnsureSceneColorCopy(ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_view);
	void DownsampleSceneColor(ID3D11ShaderResourceView* a_color, ID3D11ShaderResourceView* a_depth, const float2& a_size);
	void UpdateGlassyConstants(PerFrame& a_data, const D3D11_TEXTURE2D_DESC& a_description, const float2& a_size, bool a_hasSceneColor) const;
	void DrawDistantRain(ID3D11DeviceContext* a_context, uint32_t a_dropCount, uint32_t a_eyeCount);

	std::unique_ptr<ConstantBuffer> perFrameCB;
	std::unique_ptr<StructuredBuffer> dropBuffer;
	std::unique_ptr<StructuredBuffer> dropLocalOffsetBuffer;
	std::unique_ptr<StructuredBuffer> dropGroupOffsetBuffer;
	std::unique_ptr<StructuredBuffer> visibleDropIndexBuffer;
	std::unique_ptr<Buffer> indirectDrawArgsBuffer;
	winrt::com_ptr<ID3D11ComputeShader> rainUpdateCS;
	winrt::com_ptr<ID3D11ComputeShader> rainCountCS;
	winrt::com_ptr<ID3D11ComputeShader> rainPrefixCS;
	winrt::com_ptr<ID3D11ComputeShader> rainScatterCS;
	winrt::com_ptr<ID3D11VertexShader> rainVS;
	winrt::com_ptr<ID3D11PixelShader> rainPS;
	winrt::com_ptr<ID3D11VertexShader> distantRainVS;
	winrt::com_ptr<ID3D11PixelShader> distantRainPS;
	winrt::com_ptr<ID3D11VertexShader> sceneColorDownsampleVS;
	winrt::com_ptr<ID3D11PixelShader> sceneColorDownsamplePS;
	winrt::com_ptr<ID3D11ComputeShader> canopyOcclusionCS;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11SamplerState> refractionSampler;
	winrt::com_ptr<ID3D11ShaderResourceView> rainTextureSRV;
	float2 rainTextureSize{};
	bool rainTextureLoadAttempted = false;
	winrt::com_ptr<ID3D11Texture2D> sceneColorCopy;
	winrt::com_ptr<ID3D11RenderTargetView> sceneColorRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> sceneColorSRV;
	winrt::com_ptr<ID3D11Texture2D> sceneDepthCopy;
	winrt::com_ptr<ID3D11RenderTargetView> sceneDepthRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> sceneDepthSRV;
	D3D11_TEXTURE2D_DESC sceneColorDescription{};
	DXGI_FORMAT sceneColorViewFormat = DXGI_FORMAT_UNKNOWN;
	bool sceneColorCopyFailed = false;
	std::unique_ptr<Texture2D> solidCoverOcclusion;
	std::unique_ptr<Texture3D> canopyClassification;
	std::unique_ptr<Texture3D> canopyAccumulation;
	std::optional<bool> previousCanopyInteriorState;
	bool shaderCompileAttempted = false;
	bool canopyOcclusionShaderCompileAttempted = false;
	bool distantRainShaderCompileAttempted = false;
	bool renderPathReady = false;
	uint32_t lastWaterBlendFrame = UINT32_MAX;
	uint32_t lastDrawFrame = UINT32_MAX;
};
