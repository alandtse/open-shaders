#pragma once

#include "Buffer.h"

#include <memory>
#include <winrt/base.h>

// C4324: the aligned PerFrame cache member pads the struct
#pragma warning(push)
#pragma warning(disable: 4324)

struct Effects11 : Feature
{
public:
	virtual inline std::string GetName() override { return "Effects11"; }
	virtual inline std::string GetShortName() override { return "Effects11"; }
	virtual inline std::string GetDisplayName() override { return "Effects 11"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kPostProcessing; }
	virtual inline std::string_view GetShaderDefineName() override { return "EFFECTS11"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.effects11.description", "Effects 11 loads ENBSeries-compatible FX files, including ENB Extender presets.\nThis allows for advanced post-processing effects and visual enhancements using DirectX 11 Effect (.fx) files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries and ENB Extender preset support"),
				T("feature.effects11.key_feature_2", "DirectX 11 Effect file loading"),
				T("feature.effects11.key_feature_3", "Advanced post-processing pipeline"),
				T("feature.effects11.key_feature_4", "Custom technique execution"),
				T("feature.effects11.key_feature_5", "Dynamic UI variable system") }
		};
	}

	struct Settings
	{
		bool IgnorePresetParticles = false;
		std::string presetLocation;  // relative to game root (see PresetManager::ToRelativeKey); "" = auto-resolve
	};
	Settings settings;

	struct alignas(16) PerFrame
	{
		uint Enable;
		float ColorPow;
		float LightSpriteIntensity;
		float FireIntensity;

		float FireCurve;
		uint EnableRain;
		float RainMotionStretch;
		float RainMotionTransparency;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint EnableProceduralSun;
		float ProceduralSunDiskRadiusSq;
		float ProceduralSunDiskEdgeScale;
		float ProceduralSunGlowIntensity;

		float ProceduralSunCoronaFalloff;
		float ProceduralSunCoronaScale;
		float ProceduralSunPad[2];

		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;
		float LightSpriteCurve;
		uint EnableParticle;

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysExtinction;
		float VolumetricRaysSkyColorAmount;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;

		uint EnableCloudsScattering;
		float SkyScatteringIntensity;
		float SkyScatteringShadowAmount;
		float SkyScatteringAmount;

		float3 SkyScatteringColor;
		float SkyScatteringDustDarkening;

		float3 SkyScatteringDustTint;
		float SkyScatteringDustVolume;

		float3 SkyScatteringSunDirection;
		float SkyScatteringSunVisibility;

		float SkyScatteringHorizonRange;
		float SkyScatteringAtmosphereThickness;
		float SkyScatteringAirGlowIntensity;
		float SkyScatteringAirGlowRange;

		float SkyScatteringSunGlowIntensity;
		float SkyScatteringSunGlowRange;
		float SkyScatteringMoonGlowAmount;
		float SkyScatteringMoonGlowRange;

		float SkyScatteringSunIntensity;
		float CloudsLightingSunIntensity;
		float CloudsLightingMoonIntensity;
		uint EnableCloudsLightingFromMoon;

		uint CalculateCloudsEdgeFromScattering;
		float CloudsLightingDesaturation;
		float CloudsLightingForwardScattering;
		float CloudsLightingDensity;

		float3 CloudsColorFilter;
		float CloudsIntensity;

		float CloudsVertexAlphaBoost;
		float CloudsEdgeClamp;
		float CloudsEdgeFadePower;
		float SunBillboardTan;

		float MasserBillboardTan;
		float SecundaBillboardTan;
		float SkyScatteringPad0;
		float SkyScatteringPad1;

		float3 VolumetricFogColorFilter;
		float VolumetricFogIntensity;

		float VolumetricFogCurve;
		float VolumetricFogOpacity;
		float VolumetricFogShadowAmount;
		uint VolumetricFogEnableLighting;

		float3 VolumetricRaysSkyColor;
		float VolumetricRaysPad0;

		float StarsCurve;
		float StarsIntensity;
		float MoonCurve;
		uint EnableAnimatedStars;

		float StarsAnimationTime;
		float StarsAnimationDensity;
		float StarsAnimationIntensity;
		float AuroraIntensity;

		float AuroraCurve;
		float3 NightSkyPad;
	};
	static_assert(sizeof(PerFrame) % 16 == 0);
	static_assert(offsetof(PerFrame, StarsCurve) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricFogColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricRaysSkyColor) % 16 == 0);
	static_assert(offsetof(PerFrame, EnableCloudsScattering) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringColor) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringDustTint) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunDirection) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunIntensity) % 16 == 0);
	static_assert(offsetof(PerFrame, CloudsColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, MasserBillboardTan) % 16 == 0);

	bool enableEffect = false;

	bool volumetricRaysFailed = false;
	bool sunRaysFailed = false;
	bool tonemapFailed = false;
	winrt::com_ptr<ID3D11PixelShader> raymarchVolumetricRaysPS;
	winrt::com_ptr<ID3D11PixelShader> applyVolumetricRaysPS;
	winrt::com_ptr<ID3D11ComputeShader> blurHCS;
	winrt::com_ptr<ID3D11ComputeShader> blurVCS;
	winrt::com_ptr<ID3D11BlendState> scatteringBlendState;
	winrt::com_ptr<ID3D11BlendState> alphaBlendState;

	std::unique_ptr<Texture2D> vlTexA;
	std::unique_ptr<Texture2D> vlTexB;
	std::unique_ptr<Texture2D> vlDepthHalf;
	std::unique_ptr<ConstantBuffer> vlBlurCB;

	winrt::com_ptr<ID3D11PixelShader> sunRaysMaskPS;
	winrt::com_ptr<ID3D11PixelShader> sunRaysBlurPS;
	winrt::com_ptr<ID3D11PixelShader> sunRaysCompositePS;
	std::unique_ptr<Texture2D> sunRaysTexA;
	std::unique_ptr<Texture2D> sunRaysTexB;
	std::unique_ptr<ConstantBuffer> sunRaysCB;

	winrt::com_ptr<ID3D11Texture2D> raindropTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> raindropSRV;
	std::string raindropStatus;
	void LoadRaindropTexture();

	/** @brief Sun color after the preset's sun desaturation and filter, normalized to a peak of 1; tints the sky scattering. */
	float3 scatteringSunColor = { 1.0f, 1.0f, 1.0f };
	/** @brief Last sun direction used for sky scattering; held while the sun disc is hidden above the horizon. */
	float3 scatteringSunDirection = { 0.0f, 0.0f, 1.0f };
	bool hasScatteringSunDirection = false;

	PerFrame GetCommonBufferData();
	void UpdateSkyScattering(PerFrame& a_data);

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void Prepass() override;
	virtual void ClearShaderCache() override;

	/** @brief Resolves settings.presetLocation against PresetManager's freshly rescanned
	 *  locations and calls SetActiveLocation. Must run after both Rescan() and
	 *  LoadSettings() -- see Initialize(). */
	void ResolveActivePresetLocation();

	/** @brief One-time preset-location discovery + selection resolution; called from
	 *  SetupResources() after settings have been loaded and before EffectManager::Initialize(). */
	void Initialize();

	/** @copydoc Feature::RegisterUxActions */
	void RegisterUxActions() override;

	/** @brief Flips the "UseEffect" GLOBAL setting; bound to the Effects 11 toggle hotkey. */
	void ToggleEnabled();

	void DrawVolumetricRays();

	/** @brief Draws the ENB [RAYS] screen-space sun (or Masser) shafts additively onto the main target. */
	void DrawSunRays();
	/** @brief Raymarches and composites preset volumetric scattering. */
	void DrawVolumetricScattering();

	void OnSkyUpdateColors(RE::Sky* a_sky);
	void OverrideWeather(RE::Sky* a_sky);
	void CheckCommonData();
	/** @copydoc Feature::WantsPointLightColorOverride */
	bool WantsPointLightColorOverride() const override { return true; }
	/** @copydoc Feature::OverridePointLightColor */
	void OverridePointLightColor(float3& a_color) override;

	struct DirectionalAmbientColors
	{
		RE::NiColor directionalAmbientColors[3][2];
	};
	void OverrideAmbientLighting(DirectionalAmbientColors& DirectionalAmbientColors);

	DirectionalAmbientColors vanillaAmbientCache{};
	DirectionalAmbientColors gradedAmbientCache{};
	RE::NiColor ambientSpecularTintCache{};
	bool ambientSpecularTintCacheValid = false;
	float ambientSpecularFresnelCache = 0.0f;
	bool ambientGradeCacheValid = false;

	void ModifySky(RE::BSRenderPass* Pass);
	__declspec(noinline) void ModifyParticle(RE::BSRenderPass* Pass);
	void ParticleShaderHacks();
	/** @brief Whether the current preset enables rain rendering. */
	bool IsRainEnabled();

	/** @brief Whether Effects11 wants to replace the vanilla tonemap this frame. */
	bool WantsTonemapOwnership();
	/** @brief Runs the Effects11 chain in place of the vanilla tonemap pass. */
	bool RenderTonemap(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);
	/** @brief True when Effects11 produced the SDR scene for the frame being presented. */
	bool ReplacedTonemapperThisFrame() const;

private:
	bool EnsureScatteringBlendState();
	bool EnsureSunRaysResources(uint32_t a_width, uint32_t a_height);

	uint tonemapReplacedFrame = UINT32_MAX;  ///< frameCount at which the effect chain's last tonemap output gets presented

	/** @brief Point light settings, resolved once per frame in CheckCommonData since OverridePointLightColor runs per light. */
	struct PointLightingParams
	{
		float curve = 1.0f;
		float desaturation = 0.0f;
		float intensity = 1.0f;
	} pointLighting;

	// The feature buffer is rebuilt several times per frame, so GetCommonBufferData's lookups are replayed from here
	PerFrame perFrameCache{};
	Util::FrameChecker perFrameCacheChecker;
};

#pragma warning(pop)
