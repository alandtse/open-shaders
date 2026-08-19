#pragma once

#include "Buffer.h"

struct VanillaFresnel : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	// Metadata
	virtual inline std::string GetName() override { return "Vanilla Fresnel"; }
	virtual std::string GetDisplayName() override { return T("feature.vanilla_fresnel.name", "Vanilla Fresnel"); }
	virtual inline std::string GetShortName() override { return "VanillaFresnel"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.vanilla_fresnel.description", "Add realistic environmental reflections to vanilla materials."),
			{ T("feature.vanilla_fresnel.key_feature_1", "Add environmental reflections to all materials"),
				T("feature.vanilla_fresnel.key_feature_2", "Supports vanilla and complex materials"),
				T("feature.vanilla_fresnel.key_feature_3", "Optionally turn vanilla phong specular into GGX"),
				T("feature.vanilla_fresnel.key_feature_4", "Optionally turn static cubemaps into dynamic reflections") }
		};
	}

	// Functionality
	virtual bool inline IsCore() const override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "VANILLA_FRESNEL"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };
	// fork: shader math is per-eye and the hook is a shared vtable write, so VR needs no divergence
	virtual bool SupportsVR() override { return true; }
	virtual void PostPostLoad() override;

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	struct alignas(16) Settings
	{
		uint Enable = false;
		uint EnableGGX = false;
		uint EnableGGXOnGrass = false;
		uint EnableDynamicCubemapsConversion = false;
		uint EnableEyeSpecialHandling = true;
		float RoughnessMultiplier = 1.0f;
		float SpecularRoughnessBlend = 1.0f;
		float BaseF0Multiplier = 0.32f;
		float MinF0 = 0.02f;
		float CubemapToF0Multiplier = 1.0f;
		float ComplexMaterialF0Multiplier = 1.0f;
		float pad;
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);

	Settings settings;
};
