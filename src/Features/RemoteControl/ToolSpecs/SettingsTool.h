#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.settings. {brand} is expanded at registration. */
	inline constexpr const char* kSettingsTool =
		R"({"description":")"
		R"(Save, load, reset, or apply a performance profile to the GLOBAL {brand} user configuration, or restore one feature's saved settings. Action-dispatched on the render thread, )"
		R"(matching in-game configuration controls and active preset ownership. Global actions are fire-and-forget. loadFeature: required shortName of a loaded feature with personal settings; waits up to 5000ms and returns applied or error. Calls State::RestoreSavedFeatureSettings to reload only that feature's saved personal and user override settings over its current Base Settings and installed overrides. Refuses an unsaved scene draft for that feature; preserves other features' unsaved edits and writes no files. Verify with openshaders.feature get. save: persist current global settings (State::Save), excluding retained scene drafts. load: re-read settings from disk and apply (State::Load), refused with a localized warning while a scene draft is unsaved. reset: discard any retained scene draft, restore every feature to its defaults then persist. applyVRProfile: broadcast the named performance profile (params profile: performance|balanced|quality) through Feature::ApplyPerformanceProfile across all features (Flat and VR alike),)"
		R"( )"
		R"(then persist; )"
		R"(restart-gated fields (render preset, foveation, reprojection) take effect on next launch. Use after openshaders.feature set/reset to make changes durable, or to roll an A/B session back to the saved baseline.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"description": "Run on the render thread, including when the menu is hidden. loadFeature waits for its result and requires shortName; global actions queue. load refuses an unsaved retained scene draft; reset discards it before restoring feature defaults.",
			"enum": ["save","load","loadFeature","reset","applyVRProfile"]
		},
		"shortName": {
			"type": "string",
			"description": "Required for loadFeature: loaded feature short name from openshaders.feature list."
		},
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
