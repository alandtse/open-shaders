#include "SceneSettingsUIHooks.h"

#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"

namespace SceneSettingsUIHooks
{
	FeatureDrawGuard::FeatureDrawGuard(Feature* feature, bool, bool sceneEditing)
	{
		if (feature && !sceneEditing)
			scope = std::make_unique<SceneWidgetInterceptor::Scope>(SceneWidgetInterceptor::Context{ feature, {}, true });
	}

	FeatureDrawGuard::~FeatureDrawGuard() = default;

	void Install()
	{
		SceneWidgetInterceptor::Install();
	}
}
