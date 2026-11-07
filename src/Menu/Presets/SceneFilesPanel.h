#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace SceneFilesPanel
{
	/** @brief Selects one installed preset in the contents browser. */
	void OpenForPack(const std::string& id);
	/** @brief Draws preset files, value editing, selective copying and deletion. */
	void Draw();
	/** @brief Draws the exporter's optional individual scene-setting selection. */
	void DrawExportSelection(std::optional<std::set<std::string>>& selected, bool fromPreset,
		std::string_view search, std::optional<bool> selectAll, float visibleLines);
	struct ExportFileCounts
	{
		size_t selected;
		size_t total;
	};
	/** @brief Counts included scene files independently of the search filter. */
	ExportFileCounts GetExportFileCounts(const std::optional<std::set<std::string>>& selected, bool fromPreset);
	/** @brief Whether the current source has selected scene values to export. */
	bool HasExportSelection(const std::optional<std::set<std::string>>& selected, bool fromPreset);
	/** @brief Draws the value editor and pending confirmations once per overlay frame. */
	void DrawConfirmation();
	/** @brief Whether a pending confirmation owns input. */
	bool IsOpen();
	/** @brief Cancels pending edits and confirmations without changing files. */
	void Dismiss();
}
