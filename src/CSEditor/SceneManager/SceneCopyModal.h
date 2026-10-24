#pragma once

#include "SceneSettingsManager.h"

/// The Copy dialog a Scene Manager page opens: pick the other side, see what lands, commit.
namespace SceneCopyModal
{
	/** @brief Opens the modal for a page, optionally limiting both sides to one feature. */
	void Open(const SceneSettingsManager::SceneContextId& page, std::string_view featureShortName = {});

	/** @brief Draws the modal if this page opened it. Call from the page's toolbar every frame. */
	void Draw(const SceneSettingsManager::SceneContextId& page, std::string_view featureShortName = {});

	/** @brief Whether any other context holds settings this page could copy in. Cached per page. */
	bool HasSources(const SceneSettingsManager::SceneContextId& page, std::string_view featureShortName = {});
}
