#pragma once

struct DetailMaps : Feature
{
	virtual inline std::string GetName() override { return "Detail Maps"; }
	virtual std::string GetDisplayName() override { return T("feature.detail_maps.name", "Detail Maps"); }
	virtual inline std::string GetShortName() override { return "DetailMaps"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.detail_maps.description", "Adds fine tiling surface detail such as stone grit, wood grain, leather pores and fabric weave to textures up close, chosen by texture path rules."),
			{ T("feature.detail_maps.key_feature_1", "Works on vanilla and mod textures without editing them"),
				T("feature.detail_maps.key_feature_2", "Rules match texture paths to detail sets"),
				T("feature.detail_maps.key_feature_3", "Fades out with distance and costs nothing on other draws"),
				T("feature.detail_maps.key_feature_4", "Needs the separate Detailed Maps pack from Nexus (mod 197436)") } };
	}

	struct Settings
	{
		bool Enabled = true;
		float Tiling = 1.0f;
		float AlbedoStrength = 1.0f;
		float NormalStrength = 1.0f;
		float RoughnessStrength = 1.0f;
		float FadeStart = 400.0f;
		float FadeEnd = 1200.0f;
	} settings;

	virtual bool SupportsVR() override { return true; }
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void Prepass() override;
	virtual void Reset() override;
	virtual void PostPostLoad() override;
	virtual void GenerateShaderPermutations(RE::BSShader* a_shader) override;
	virtual uint32_t GetLightingVariantFlag() const override;
	virtual std::string_view GetLightingVariantDefine() const override;
	virtual void RegisterUxActions() override;

	/**
	 * @brief Swaps in the detail Lighting variant for the render pass the engine just set up, when a rule matches.
	 * @param a_shader The lighting shader that set up the pass.
	 * @param a_pass The render pass about to draw.
	 */
	void BeginPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);

	/** @brief Restores the engine's pixel shader and releases the detail texture after a detail draw. */
	void EndPass();

private:
	struct Hooks;

	struct Set
	{
		std::string name;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
	};

	struct Rule
	{
		std::vector<std::string> match;
		std::vector<std::string> exclude;
		uint32_t set = 0;
		float scale = 4.0f;
		float albedo = 1.0f;
		float normal = 1.0f;
		float roughness = 1.0f;
		bool transpose = false;
		int priority = 0;
		size_t file = 0;
	};

	struct Entry
	{
		RE::BSFixedString name;
		int32_t rule = -1;
		const RE::BSTextureSet* fallbackSet = nullptr;
	};

	bool HasContent() const { return !sets.empty() && !rules.empty(); }
	bool ShouldPrecompile(uint32_t a_descriptor) const;
	bool IsEligible(uint32_t a_descriptor) const;
	void Reload();
	json BuildStatus() const;
	void ForgetLastRule();
	void LoadSets();
	void LoadRules();
	int32_t FindRule(RE::BSRenderPass* a_pass);
	int32_t MatchRule(const char* a_path) const;
	void ClampSettings();

	std::vector<Set> sets;
	std::vector<Rule> rules;
	std::vector<std::string> excludes;
	std::vector<std::string> ruleErrors;
	ankerl::unordered_dense::map<RE::NiSourceTexture*, Entry> entries;
	RE::NiSourceTexture* lastDiffuse = nullptr;
	const RE::BSTextureSet* lastSet = nullptr;
	int32_t lastRule = -1;
	winrt::com_ptr<ID3D11PixelShader> savedPixelShader;
	uint32_t passCount = 0;
	uint32_t lastPassCount = 0;
	bool swapped = false;
};
