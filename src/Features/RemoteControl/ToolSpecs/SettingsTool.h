#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.settings. {brand} is expanded at registration. */
	inline constexpr const char* kSettingsTool =
		R"({"description":")"
		R"(Save, load, reset, or apply a performance profile to the GLOBAL {brand} user configuration, or restore one feature's saved settings. Action-dispatched on the render thread, )"
		R"(matching in-game configuration controls and active preset ownership. Save, load, reset and profile actions queue on the render thread. loadFeature: required shortName of a loaded feature with personal settings; waits up to 5000ms and returns applied or error. Calls State::RestoreSavedFeatureSettings to reload only that feature's saved personal settings over its current Base Settings. Refuses an unsaved scene draft for that feature; preserves other features' unsaved edits and writes no files. Verify with openshaders.feature get. )"
		R"(status: read canRestoreSaved, canReapplyBaseSettings, canUndo and canRedo without changing settings; optional shortName and currentTab narrow restoration availability to a feature and its active tab. reapplyBaseSettings: use all enabled Base Settings values instead of personal edits to those settings, preserving unrelated edits and writing no files. Both wait for their render-thread result. )"
		R"(save: persist current global settings, excluding retained scene drafts. load: re-read settings from disk, refusing unsaved scene drafts. reset: discard retained scene drafts, restore every feature's defaults, then persist. applyVRProfile: apply profile performance|balanced|quality through Feature::ApplyPerformanceProfile across all features, then persist. Restart-gated fields take effect on the next launch. Use load to restore saved settings after an A/B session.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"description": "Run on the render thread, including when the menu is hidden. status, reapplyBaseSettings and loadFeature wait for their result; loadFeature requires shortName. Other actions queue. load refuses an unsaved retained scene draft; reset discards it before restoring feature defaults.",
			"enum": ["save","load","loadFeature","reset","applyVRProfile","status","reapplyBaseSettings"]
		},
		"shortName": {
			"type": "string",
			"description": "Loaded feature short name from openshaders.feature list. Required for loadFeature; optional for status."
		},
		"currentTab": {"type":"boolean", "description":"status only: restrict restoration availability to the selected feature tab"},
		"profile": {
			"type": "string",
			"enum": ["performance","balanced","quality"]
		}
	},
	"required": ["action"],
	"oneOf": [
		{
			"properties": {
				"action": {"enum":["loadFeature"]},
				"shortName": {"minLength":1}
			},
			"required": ["shortName"]
		},
		{
			"properties": {
				"action": {"not":{"enum":["loadFeature"]}}
			}
		}
	]
})"
		R"(})";
}
