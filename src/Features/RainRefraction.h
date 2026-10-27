#pragma once

#include "Buffer.h"
#include "GpuPass.h"
#include "RainRefraction/TextureFootprint.h"

#include <winrt/base.h>

/**
 * @brief Renders rain as refractive drops that reflect and refract the Dynamic Cubemaps environment.
 * While Effects 11 is active its preset owns the settings and supplies the raindrop texture.
 */
struct RainRefraction : Feature
{
public:
	virtual bool IsCore() const override { return true; }
	virtual bool SupportsVR() override { return true; }

	virtual inline std::string GetName() override { return "Rain Refraction"; }
	virtual std::string GetDisplayName() override { return T("feature.rain_refraction.name", "Rain Refraction"); }
	virtual inline std::string GetShortName() override { return "RainRefraction"; }
	virtual inline std::string_view GetShaderDefineName() override { return "RAIN_REFRACTION"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Particle; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.rain_refraction.description", "Renders rain as refractive drops that reflect and refract the surrounding environment."),
			{ T("feature.rain_refraction.key_feature_1", "Raindrop normal map with Fresnel reflection and refraction from Dynamic Cubemaps"),
				T("feature.rain_refraction.key_feature_2", "Adjustable motion stretch and transparency"),
				T("feature.rain_refraction.key_feature_3", "Driven by the Effects 11 preset and its enbraindrops.png while Effects 11 is active") } };
	}

	static constexpr float kMinMotionStretch = 0.01f;
	static constexpr float kMaxMotionStretch = 3.0f;

	/** @brief User settings, used while Effects 11 is not active; mirrors the preset's [RAIN] Enable, MotionStretch and MotionTransparency. */
	struct Settings
	{
		bool Enable = false;
		float MotionStretch = 2.0f;
		float MotionTransparency = 0.91f;
	} settings;

	/** @brief Feature buffer data, matching RainRefractionSettings in SharedData.hlsli; Enable is cleared when the raindrop texture or blend state is missing. */
	struct PerFrame
	{
		uint Enable;
		float MotionStretch;
		float MotionTransparency;
		float pad0;
		float4 TextureBounds{ 0, 0, 1, 1 };
		float2 TextureRadiusScale{ 1, 1 };
		float2 pad1{};
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	/** @brief Supplies the rain shader data while a preset owns the rain settings, or nothing otherwise. */
	using PresetResolver = std::function<std::optional<PerFrame>()>;

	/** @brief A loaded raindrop texture, or the reason it failed to load. */
	struct RaindropTexture
	{
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		RainRefractionTexture::Footprint footprint;
		std::string status;  ///< Failure reason; empty when loaded
	};

	RaindropTexture defaultRaindrops;  ///< Bundled with the feature
	RaindropTexture presetRaindrops;   ///< enbraindrops.png from the Effects 11 preset

	PresetResolver presetResolver;
	bool presetManaged = false;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
	/** @brief Loads the bundled default raindrop texture. */
	virtual void SetupResources() override;
	/** @brief Enables GPU timing around rain render passes. */
	bool WantsRenderPassHook() const override { return true; }
	/** @copydoc Feature::OnRenderPassBegin */
	std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* a_pass) override;

	/** @brief Lets a preset provider (Effects 11) take over the rain settings; this feature never reads that provider directly. */
	void SetPresetResolver(PresetResolver a_resolver);

	/** @brief Shader data for the current frame, taken from the preset resolver while it supplies data. */
	PerFrame GetCommonBufferData();
	/** @brief Returns true while a preset owns the rain settings, as of the last GetCommonBufferData. */
	bool IsManagedByENB() const;
	/** @brief The preset's raindrop texture while managed by ENB and it loaded, otherwise the bundled default; null if neither loaded. */
	ID3D11ShaderResourceView* GetRaindropSRV() const;
	/** @brief Loads enbraindrops.png from the Effects 11 preset folder; called whenever a preset is applied. */
	void LoadPresetRaindrops(const std::filesystem::path& a_presetPath);

	/** @brief Binds shared vertex buffers for rain and the normal map while refraction is active. */
	void ModifyParticle();
	/** @brief Switches refractive rain passes to alpha blending. */
	void ParticleShaderHacks();
	/** @brief Binds rain draw data; failure binds native defaults and returns false. */
	bool BindRenderSpace(float2 a_rasterSize, float2 a_sourceScale, bool a_testDepth);
	/** @brief Clears scene-copy and draw resources and their failure latches. */
	void ClearShaderCache() override;

private:
	struct DrawData
	{
		float2 sourceScale{ 1, 1 };
		float2 rasterSize{};
		uint32_t testDepth = 0;
		uint32_t sceneAvailable = 0;
		float2 padding{};
	};
	STATIC_ASSERT_ALIGNAS_16(DrawData);

	bool CaptureSceneColor();

	/** @brief Returns true when the current particle pass is rain and refractive rain is active this frame. */
	bool IsRefractiveRainPass();

	winrt::com_ptr<ID3D11BlendState> alphaBlendState;
	std::optional<ScopedGpuPass> particlePass;
	std::unique_ptr<Texture2D> sceneColor;
	std::array<std::unique_ptr<ConstantBuffer>, 2> drawCB;
	std::array<DrawData, 2> drawData{};
	uint32_t sceneFrame = UINT32_MAX;
	bool sceneCopyFailed = false;
	bool drawDataFailed = false;

	// The feature buffer is rebuilt several times per frame and the particle hooks run per pass
	PerFrame perFrameCache{};
	Util::FrameChecker perFrameCacheChecker;
};
