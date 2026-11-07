#pragma once

#include "SceneSettingsManager.h"

/// The Copy dialog a Scene Manager page opens: pick the other side, see what lands, commit.
namespace SceneCopyModal
{
	/** @brief Opens the modal for a page, optionally limiting both sides to one feature. */
	void Open(const SceneSettingsManager::SceneContextId& page, std::string_view featureShortName = {}, SceneSettingsManager::SettingSelection selection = std::nullopt);

	/** @brief Draws the modal if this page opened it. Call from the page's toolbar every frame. */
	void Draw(const SceneSettingsManager::SceneContextId& page, std::string_view featureShortName = {});

	/** @brief Starts a copy session hosted by the universal Manage Settings dialog. */
	void PrepareEmbedded();
	/** @brief Draws standalone source selection and destination review inside Manage Settings. */
	void DrawEmbedded();

	/** @brief Whether any other context holds settings this page could copy in. Cached per page. */
	bool HasSources(const SceneSettingsManager::SceneContextId& page, std::string_view featureShortName = {});
}
