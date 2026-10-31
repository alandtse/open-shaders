#pragma once

#include <string>

/// Preset authoring hosted by the Presets page, with upstream pack formats and export safeguards.
namespace PresetExportPanel
{
	/// Whether scene, form or base settings can be exported.
	bool CanExport();

	/** @brief Describes the shared Presets export workflow. */
	const char* GetSummary();

	/// Opens the Presets page at preset authoring.
	void Open();
	/** @brief Draws preset contents, metadata, artwork and compatibility on the Presets page. */
	void DrawPage();
	/** @brief Opens authoring with an installed pack's metadata and selected contents. */
	void OpenForPack(const std::string& packId);
	/** @brief Returns whether export or its confirmation still owns input. */
	bool IsOpen();

	/**
	 * @brief Opens Presets with Base Settings selected as the export type.
	 * @param featureShortName Feature ticked to begin with, such as the one open in Base Settings.
	 */
	void OpenBaseline(const std::string& featureShortName = {});

	/// Draws confirmations and setting selectors once per overlay frame.
	void Draw();
}
