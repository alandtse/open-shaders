#include "SceneManagerUI.h"

#include "CSEditor/SceneManager/SceneSettingsUI.h"
#include "I18n/I18n.h"

namespace SceneManagerUI
{
	bool CanEditFeaturePage(Feature* feature)
	{
		return SceneSettingsUI::CanEditFeaturePage(feature);
	}

	bool BeginFeaturePageEditing(Feature* feature)
	{
		return SceneSettingsUI::BeginFeaturePageEditing(feature);
	}

	bool IsFeaturePageEditing(Feature* feature)
	{
		return SceneSettingsUI::IsFeaturePageEditing(feature);
	}

	bool DrawFeaturePageControls(Feature* feature, bool enabled)
	{
		return SceneSettingsUI::DrawFeaturePageControls(feature, enabled);
	}

	void HideFeaturePageEditing()
	{
		SceneSettingsUI::HideFeaturePageEditing();
	}

	void Draw()
	{
		SceneSettingsUI::DrawStandalonePage();
	}
}
