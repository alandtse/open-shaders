#pragma once

struct Feature;

namespace SceneManagerUI
{
	void Draw();
	bool CanEditFeaturePage(Feature* feature);
	bool BeginFeaturePageEditing(Feature* feature);
	bool IsFeaturePageEditing(Feature* feature);
	bool DrawFeaturePageControls(Feature* feature, bool enabled);
	void EndFeaturePageEditing(bool storeChanges = true);
}
