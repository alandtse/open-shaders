#pragma once

#include "SceneSettingsManager.h"

/// Actions for one scene context or feature: pause/resume, copy to or from another context
/// (picked in the Copy modal), and clear page. Export is editor-wide and lives on the action bar.
namespace ScenePageToolbar
{
	/// Draws the actions and their dialogs right-aligned on the current row (precede with SameLine to share it).
	/// The context's period names the saved set the page authors: Count for a flat set.
	/** @brief Empty featureShortName acts on the whole context; otherwise actions affect only that feature. */
	void Draw(const SceneSettingsManager::SceneContextId& context, std::string_view featureShortName = {});
}
