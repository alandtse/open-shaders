#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct Feature;

/// Included-setting selection and existing-file edits owned by Presets.
namespace BaseSettingsEditor
{
	/** @brief Opens one existing Base Settings file for changes to its included settings. */
	void BeginExportInto(Feature* feature, const std::string& packId);
	/** @brief Selects one feature's settings for the current preset export without writing files. */
	void SelectSettings(Feature* feature, const std::optional<std::vector<std::string>>& selected,
		std::function<void(std::vector<std::string>)> apply);
	/** @brief Returns the current file's selected export paths, or null when it cannot be read. */
	std::optional<std::vector<std::string>> GetIncludedSettings(Feature* feature, const std::string& packId);
	/** @brief Draws the active selector and its results once per overlay frame. */
	void DrawExport();
	/** @brief Whether the selector currently owns input. */
	bool IsOpen();
	/** @brief Whether this feature serializes settings that a preset can include. */
	bool HasExportableSettings(Feature* feature);
}
