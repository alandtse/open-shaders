#pragma once

#include <optional>
#include <string>

#include "../UndoHistory.h"
#include "RE/B/BSCoreTypes.h"  // RE::FormID
#include "SceneSettingsManager.h"

/// Scene authoring shared by the standalone menu page and feature top bars.
struct Feature;

namespace SceneSettingsUI
{
	/** @brief Draws the standalone Scene Manager page. */
	void DrawStandalonePage();
	/** @brief Draws scene authoring controls above a feature page. */
	bool DrawFeaturePageControls(Feature* feature, bool enabled);
	bool CanEditFeaturePage(Feature* feature);
	bool BeginFeaturePageEditing(Feature* feature);
	bool IsFeaturePageEditing(Feature* feature);
	void HideFeaturePageEditing();
	/** @brief Describes the explicit-save requirement when a retained draft blocks a reload. */
	const char* PendingContextEditMessage();
	/** @brief Shows the retained-draft warning without changing any selection. */
	void WarnPendingContextEdit();
	/** @brief Starts or stops the selected toolbar context's environment preview. */
	bool SetFeaturePagePreviewPlaying(bool playing);
	/** @brief Draws the selected scene context using the feature's own controls. */
	void DrawFeaturePageBody(Feature* feature);

	/** @brief Opens the main-menu page authoring a scene context, with a feature selected. */
	void OpenSceneContext(const SceneSettingsManager::SceneContextId& context, const std::string& featureShortName);

	/**
	 * @brief Whether the page authoring a layer lists a feature, so opening it can select that feature.
	 * Weather pages list the transitionable set; the other layers list every scene feature.
	 */
	bool LayerListsFeature(SceneSettingsManager::SceneContextType layer, const std::string& featureShortName);

	/**
	 * @brief The context of a layer that applies where the player is now: the live period, the active
	 * weather, the interior layer, or the narrowest place in the live chain on the user's Locations list.
	 * @return Nothing when the layer cannot hold the feature or nothing matches here.
	 */
	std::optional<SceneSettingsManager::SceneContextId> ResolveCurrentLayer(
		SceneSettingsManager::SceneContextType layer, const std::string& featureShortName);

	/** @brief ResolveCurrentLayer for any feature: nothing only when no weather is active or no listed place matches. */
	std::optional<SceneSettingsManager::SceneContextId> ResolveCurrentContext(SceneSettingsManager::SceneContextType layer);

	/** @brief Opens the standalone Locations page to add a place to the list. */
	void OpenLocationsPage();

	/** @brief Opens the standalone Scene Manager page, keeping the selected feature. */
	void OpenSceneManagerPage();

	/**
	 * @brief Ensures an override for the player's current place and opens its editor on a feature.
	 *
	 * Used when a Weather/Time-of-Day/Interior control cannot hold a setting: land on the narrowest
	 * place in the live chain with that feature selected, so the value is editable immediately.
	 * Shows a fade toast and returns false when the place cannot be opened or cannot hold the setting.
	 */
	bool OpenCurrentLocationForSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);

	/** @brief The undo target for the user's scene settings, every context at once. */
	UndoHistory::Target MakeUndoTarget();
}
