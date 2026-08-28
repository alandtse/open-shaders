#pragma once

#include "Feature.h"
#include "I18n/I18n.h"

/** Hosts the always-on shared wind controls on a dedicated feature page. */
struct Wind : Feature
{
	virtual std::string GetName() override { return "Wind"; }
	virtual std::string GetDisplayName() override { return T("feature.wind.name", "Wind"); }
	virtual std::string GetShortName() override { return "Wind"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFoliage; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.wind.description", "Shared ambient, tree, grass, and transient wind controls."),
			{ T("feature.wind.key_feature_1", "Procedural ambient gust field"),
				T("feature.wind.key_feature_2", "Tree and grass response tuning"),
				T("feature.wind.key_feature_3", "Transient wind impulses") } };
	}

	/** @copydoc Feature::DrawSettings */
	virtual void DrawSettings() override;
	/** @return true because Wind restores only the active settings tab. */
	virtual bool HasScopedDefaultSettings() const override { return true; }
	/** Restores defaults for the active Wind tab. */
	virtual void RestoreCurrentPageDefaultSettings() override;
	/** @return true because Wind reapplies overrides only for the active settings tab. */
	virtual bool HasScopedOverrideSettings() const override { return true; }
	/** Reapplies override-controlled settings for the active Wind tab. */
	virtual bool ReapplyCurrentPageOverrideSettings() override;
};
