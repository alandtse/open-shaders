#pragma once

struct LinearLighting : Feature
{
	static LinearLighting* GetSingleton()
	{
		static LinearLighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Linear Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.linear_lighting.name", "Linear Lighting"); }
	virtual inline std::string GetShortName() override { return "LinearLighting"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.linear_lighting.description", "Linear Lighting does internal color space conversion to improve lighting calculation accuracy."),
			{ T("feature.linear_lighting.key_feature_1", "Semantic authored-color conversion"),
				T("feature.linear_lighting.key_feature_2", "Corrects lighting calculations"),
				T("feature.linear_lighting.key_feature_3", "Makes PBR really work") } };
	};

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };

	struct Settings
	{
		uint enableLinearLighting = false;
		uint enableACEScg = false;
		float conversionSaturation = 0.92f;

		// Lighting multipliers
		float ambientMult = 0.32f;
	} settings;

	struct alignas(16) PerFrameData
	{
		uint enableLinearLighting;
		uint enableACEScg;
		uint isDirLightLinear;
		float dirLightMult;
		float diffuseGamma;
		float diffuseCurve;
		float diffuseWhiteReflectance;
		float conversionSaturation;
		RE::NiColor effectLightingColor;
		float ambientMult;
		RE::NiColor skyStaticsColor;
		float pad;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 0x40);
	static_assert(offsetof(PerFrameData, conversionSaturation) == 0x1C);
	static_assert(offsetof(PerFrameData, skyStaticsColor) == 0x30);

	static constexpr std::array<float, 3> kNoProjectedMaterialColorScale{ -1.0f, -1.0f, -1.0f };

	struct alignas(16) PerGeometryData
	{
		float emissiveMult;
		std::array<float, 3> projectedMaterialColorScale = kNoProjectedMaterialColorScale;
	};
	static_assert(sizeof(PerGeometryData) == 16);

	std::array<std::array<float, 3>, 2> lodProjectedMaterialColorScales{ kNoProjectedMaterialColorScale, kNoProjectedMaterialColorScale };

	ConstantBuffer* PerGeometryCB = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> sceneGammaDecodeCS;
	bool sceneGammaActive = false;

	uint isDirLightLinear = false;
	float dirLightMult = 1.0f;
	RE::NiColor effectLightingColor{ 1.0f, 1.0f, 1.0f };
	RE::NiColor skyStaticsColor{ 1.0f, 1.0f, 1.0f };
	RE::NiColor weatherEffectLightingSource{};
	RE::NiColor weatherSkyStaticsSource{};
	bool weatherLightingColorsInitialized = false;
	float weatherConversionSaturation = 1.0f;

	/** @brief Draws the Linear Lighting controls and lighting multipliers. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Reads the directional light multiplier and projected LOD material colors. */
	virtual void Prepass() override;
	/** @brief Installs the lighting geometry hook. */
	virtual void PostPostLoad() override;

	/** @brief Creates the emissive data buffer and compiles the scene gamma decode shader. */
	virtual void SetupResources() override;
	/** @brief Recompiles the scene gamma decode shader after a shader-cache clear. */
	virtual void ClearShaderCache() override;
	/** @brief Marks kMAIN as gamma-domain storage for the main world-rendering interval. */
	virtual void OnWorldRenderBegin() override;
	/** @brief Decodes the completed gamma-domain world scene in place. */
	virtual void OnWorldRenderEnd(RE::RENDER_TARGET a_renderTarget) override;
	/** @brief Finishes any pending scene decode before post-processing consumes its input. */
	virtual void OnBeforePostProcessing(RE::RENDER_TARGET a_renderTarget) override;
	/** @brief Uses a linear clear color and target for cubemap rendering, restoring both on scope exit. */
	virtual std::function<void()> OnReflectionsRenderBegin() override;

	/** @brief Populates and returns the per-frame constant buffer data with gamma and multiplier settings. */
	PerFrameData GetCommonBufferData();
	/** @brief Returns whether the engine and shaders should currently use linear lighting data. */
	bool IsLinearLightingActive() const;
	/** @brief Compiles the scene gamma decode shader when the target format supports typed UAV loads. */
	void CompileSceneGammaDecodeShader();

	/** @brief Caches linear copies of the interpolated weather colors used by effect meshes. */
	virtual void OnWeatherColorsUpdated(RE::Sky* a_sky) override;

	/**
	 * @brief Decodes an authored Skyrim color into linear sRGB.
	 * @param inColor The input color in gamma space.
	 * @param saturation Luminance-preserving saturation of the decoded input.
	 * @return The color converted to linear space.
	 */
	static RE::NiColor DecodeAuthoredColor(RE::NiColor inColor, float saturation = 1.0f);

	/**
	 * @brief Uploads emissive and projected material data during lighting geometry setup.
	 * @param a_pass The render pass whose lighting properties to read.
	 */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);

	/** @brief Contains the lighting shader hook implementation. */
	struct Hooks;
};
