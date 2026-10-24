#pragma once

#include <memory>

struct Feature;
namespace SceneWidgetInterceptor
{
	class Scope;
}

namespace SceneSettingsUIHooks
{
	/** @brief Scopes the main feature body's generic settings interception. */
	class FeatureDrawGuard
	{
	public:
		FeatureDrawGuard(Feature* feature, bool sceneControlled, bool sceneEditing = false);
		~FeatureDrawGuard();
		FeatureDrawGuard(const FeatureDrawGuard&) = delete;
		FeatureDrawGuard& operator=(const FeatureDrawGuard&) = delete;

	private:
		std::unique_ptr<SceneWidgetInterceptor::Scope> scope;
	};

	/** @brief Installs the shared widget interceptor. */
	void Install();
}
