#pragma once

#include "Feature.h"

/** @brief Exposes the shared tree and grass foliage lighting controls. */
struct FoliageLighting : Feature
{
	struct alignas(16) Settings
	{
		uint EnableFoliageScattering = 1;
		uint EnableFoliageAmbientBoost = 0;
		uint EnableFoliageAmbientFlip = 1;
		float FoliageAmbientAmount = 0.25f;
		uint EnableGrassScattering = 1;
		uint pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);
	static_assert(offsetof(Settings, EnableFoliageAmbientBoost) == sizeof(uint));
	static_assert(offsetof(Settings, EnableFoliageAmbientFlip) == sizeof(uint) * 2);
	static_assert(offsetof(Settings, FoliageAmbientAmount) == sizeof(uint) * 3);
	static_assert(offsetof(Settings, EnableGrassScattering) == sizeof(uint) * 4);
	static_assert(sizeof(Settings) == 32);

	virtual std::string GetName() override { return "Foliage Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.foliage_lighting.name", "Foliage Lighting"); }
	virtual std::string GetShortName() override { return "FoliageLighting"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFoliage; }
	virtual bool IsCore() const override { return true; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.foliage_lighting.description", "Adds inexpensive transmission and ambient controls for animated foliage."),
			{ T("feature.foliage_lighting.key_feature_1", "View-dependent foliage transmission"),
				T("feature.foliage_lighting.key_feature_2", "Tree backside ambient controls"),
				T("feature.foliage_lighting.key_feature_3", "Independent grass scattering toggle") } };
	}

	/** @brief Draws the shared tree and grass foliage lighting controls. */
	virtual void DrawSettings() override;
	/** @brief Serializes foliage lighting settings. */
	virtual void SaveSettings(json& o_json) override;
	/** @brief Loads foliage lighting settings. */
	virtual void LoadSettings(json& o_json) override;
	/** @brief Restores default foliage lighting settings. */
	virtual void RestoreDefaultSettings() override;

	Settings settings;
};
