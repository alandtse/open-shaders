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
				T("feature.detail_maps.key_feature_3", "Fades out with distance and costs nothing on other draws") } };
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

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void Reset() override;

	bool ShouldPrecompile(uint32_t a_descriptor) const;
	bool IsEligible(uint32_t a_descriptor) const;
	bool BindPass(RE::BSRenderPass* a_pass);
	void InvalidateBindings() { bindingValid = false; }

private:
	struct PerDraw
	{
		float Scale;
		float Albedo;
		float Normal;
		float Roughness;
		float FadeEnd;
		float FadeInvRange;
		float Transpose;
		float Pad;
	};
	static_assert(sizeof(PerDraw) == 32);

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
	};

	void Reload();
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
	int32_t lastRule = -1;

	eastl::unique_ptr<ConstantBuffer> perDrawCB;
	PerDraw lastPerDraw{};
	ID3D11ShaderResourceView* boundView = nullptr;
	bool bindingValid = false;
	uint32_t passCount = 0;
	uint32_t lastPassCount = 0;
};
