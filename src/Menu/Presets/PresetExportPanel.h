#pragma once

#include <optional>
#include <string>
#include <vector>

/// Preset authoring hosted by the Presets page, with upstream pack formats and export safeguards.
namespace PresetExportPanel
{
	/// Whether scene, form or base settings can be exported.
	bool CanExport();

	/// Opens the Presets page at preset authoring.
	void Open();
	/** @brief Draws preset contents, metadata, artwork and compatibility on the Presets page. */
	void DrawPage();
	/** @brief Returns whether export or its confirmation still owns input. */
	bool IsOpen();
	/** @brief Cancels pending export and selection dialogs without writing files. */
	void Dismiss();

	/**
	 * @brief Opens Presets with Base Settings selected as the export type.
	 * @param featureShortName Feature ticked to begin with, such as the one open in Base Settings.
	 */
	void OpenBaseline(const std::string& featureShortName = {}, std::optional<std::vector<std::string>> pageKeys = std::nullopt);

	/// Draws confirmations and setting selectors once per overlay frame.
	void Draw();
}
