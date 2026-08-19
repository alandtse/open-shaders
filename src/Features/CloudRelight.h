#pragma once

/** @brief Relights vanilla clouds from the active directional light. */
struct CloudRelight : Feature
{
	/** @brief Per-frame Cloud Relight settings shared with HLSL. */
	struct alignas(16) Settings
	{
		uint enabled = false;
		float cloudRelightMix = 1.0f;
		float cloudOriginalMix = 0.68f;
		float silverLiningMix = 1.0f;

		float silverLiningSpread = 0.0f;
		float pad[3] = {};
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);

	Settings settings;

	/** @brief Returns the stable feature name. */
	virtual inline std::string GetName() override { return "Cloud Relight"; }
	/** @brief Returns the localized feature name. */
	virtual std::string GetDisplayName() override { return T("feature.cloud_relight.name", "Cloud Relight"); }
	/** @brief Returns the shader configuration name. */
	virtual inline std::string GetShortName() override { return "CloudRelight"; }
	/** @brief Returns the menu category. */
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	/** @brief Returns the shader permutation define. */
	virtual inline std::string_view GetShaderDefineName() override { return "CLOUD_RELIGHT"; }
	/** @brief Enables the feature define for sky shaders. */
	virtual inline bool HasShaderDefine(RE::BSShader::Type shaderType) override { return shaderType == RE::BSShader::Type::Sky; }
	/** @brief Reports VR support. */
	virtual bool SupportsVR() override { return true; }

	/** @brief Returns the localized feature summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.cloud_relight.description", "Relights vanilla cloud textures using the active directional light, cloud self-shadowing, and silver-lining phase lighting."),
			{ T("feature.cloud_relight.key_feature_1", "Vanilla cloud color and relit cloud color blending"),
				T("feature.cloud_relight.key_feature_2", "Silver-lining and forward-scattering phase lighting"),
				T("feature.cloud_relight.key_feature_3", "Directional cloud self-shadowing from the active sun or moon") }
		};
	}

	/** @brief Draws Cloud Relight settings. */
	virtual void DrawSettings() override;
	/** @brief Loads and validates Cloud Relight settings. */
	virtual void LoadSettings(json& o_json) override;
	/** @brief Saves Cloud Relight settings. */
	virtual void SaveSettings(json& o_json) override;
	/** @brief Restores default Cloud Relight settings. */
	virtual void RestoreDefaultSettings() override;

	/** @brief Returns the settings uploaded to the shared feature buffer. */
	Settings GetCommonBufferData() const;
};
