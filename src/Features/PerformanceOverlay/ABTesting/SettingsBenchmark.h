#pragma once

#include "SceneSettingsCatalog.generated.h"
#include "SettingsBenchmarkEngine.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

struct Feature;

/**
 * @brief In-headset A/B benchmark for renderer settings.
 *
 * Applies each selected variant (a partial settings patch) over the user's current settings, measures
 * the SteamVR compositor's application GPU time between baseline blocks, and restores the original
 * settings when the run finishes, is cancelled or fails. Nothing is saved to disk except the results file
 * written next to the log. VR only: the timing comes from the compositor.
 */
namespace SettingsBenchmark
{
	using json = nlohmann::json;

	/** @brief Partial settings for one feature, in the shape that feature saves. */
	struct Patch
	{
		std::string feature;  ///< Feature short name, e.g. "Upscaling".
		json settings;
	};

	struct Variant
	{
		std::string id;
		std::string label;
		std::string group;
		std::string note;
		std::vector<Patch> patches;
	};

	/** @brief The built-in variants. */
	const std::vector<Variant>& Variants();

	/** @brief Run settings the menu and devbench edit before a start. */
	Config& Configuration();

	/**
	 * @brief Starts a run.
	 * @param variantIds Variants to run in order; empty runs every built-in variant.
	 * @param error Receives the reason when the run cannot start.
	 */
	bool Start(const std::vector<std::string>& variantIds, std::string* error);

	/**
	 * @brief Starts a one-variant run that changes a single catalogued setting and leaves the rest alone.
	 * @param target The value to measure, in the shape the setting is saved in.
	 * @param label Variant name shown in the results.
	 */
	bool StartForSetting(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const json& target, std::string label, std::string* error);

	/** @brief Finds a catalogued setting by feature short name, serialized key and serialized path (empty for a top-level key), then runs StartForSetting. */
	bool StartForSettingKey(const std::string& featureShortName, const std::string& key, const std::string& path, const json& value, std::string* error);

	/** @brief Cancels a running benchmark and restores the original settings. */
	void Cancel();

	bool IsRunning();

	/** @brief Phase, progress and results for devbench. */
	json StatusJson();

	/** @brief Once per rendered frame, on the render thread. */
	void Tick();

	/** @brief ImGui panel for the Advanced page. */
	void DrawPanel();

	/** @brief Small progress window while a run is active, so it can be followed with the menu closed. */
	void DrawOverlayUI();
}
