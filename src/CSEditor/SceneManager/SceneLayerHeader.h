#pragma once

#include <string>

#include "SceneSettingsManager.h"

/// Scene headers highlight the edited layer so the panel's write target is clear.
/// Other applicable layers link to their pages when they list the selected feature.
namespace SceneLayerHeader
{
	/** @brief Draws the preset provider's handoff control, returning whether it was shown. */
	bool DrawPresetHandoff(const std::string& featureShortName);

	/**
	 * @brief Header for the base layer: the stack with Base highlighted, and the scene layers
	 * overriding this feature right now, each linking to the page that authors it.
	 * @param featureShortName Feature whose base settings the panel below edits; the scene layers in
	 * the stack open on it.
	 */
	void DrawBase(const std::string& featureShortName);

	/**
	 * @brief Header for a scene layer: the stack with the context's layer highlighted.
	 * @param context Context the panel below writes to; its period names the set being edited.
	 * @param featureShortName Feature the panel shows; the other layers in the stack open on it.
	 */
	void DrawScene(const SceneSettingsManager::SceneContextId& context, const std::string& featureShortName);

	/** @brief Every layer of the stack the player is in, what applies on it now and how many settings it overrides across all features; each scene layer links to its page. */
	void DrawSummary();
}
