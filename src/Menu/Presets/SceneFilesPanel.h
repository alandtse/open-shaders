#pragma once

#include <optional>
#include <set>
#include <string>

namespace SceneFilesPanel
{
	/** @brief Selects one installed preset in the contents browser. */
	void OpenForPack(const std::string& id);
	/** @brief Draws preset files, selective copying and deletion. */
	void Draw();
	/** @brief Draws the exporter's optional individual scene-setting selection. */
	void DrawExportSelection(std::optional<std::set<std::string>>& selected);
	/** @brief Draws pending confirmations once per overlay frame. */
	void DrawConfirmation();
	/** @brief Whether a pending confirmation owns input. */
	bool IsOpen();
}
