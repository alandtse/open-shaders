#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.settings. {brand} is expanded at registration. */
	inline constexpr const char* kSettingsTool =
		R"({"description":")"
		R"(Save, load, reset, or apply a performance profile to the GLOBAL {brand} user configuration. Action-dispatched, )"
		R"(all fire-and-forget on the main thread. save: persist current settings (State::Save). load: re-read settings from disk and apply (State::Load). reset: restore every feature to its defaults then persist. applyVRProfile: broadcast the named performance profile (params profile: performance|balanced|quality) through Feature::ApplyPerformanceProfile across all features (Flat and VR alike),)"
		R"( )"
		R"(then persist; )"
		R"(restart-gated fields (render preset, foveation, reprojection) take effect on next launch. Use after openshaders.feature set/reset to make changes durable, or to roll an A/B session back to the saved baseline.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": ["save","load","reset","applyVRProfile"]
		},
		"profile": {
			"type": "string",
			"enum": ["performance","balanced","quality"]
		}
	},
	"required": ["action"]
})"
		R"(})";
}
