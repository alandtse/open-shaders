#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.actions. {brand} is expanded at registration. */
	inline constexpr const char* kActionsTool =
		R"({"description":")"
		R"(Devbench-registered one-shot commands and read-only queries -- the imperative/derived-state counterpart to openshaders.feature's settings get/set. A feature exposes these via FEATURE_COMMAND/FEATURE_QUERY in its Feature::RegisterUxActions() override (see Utils/DevBenchUx.h); )"
		R"(every feature also gets two built-in queries for free: matchesPerformanceProfile and profilePreviewText (params: profile=performance|balanced|quality), )"
		R"(mirroring Feature::MatchesPerformanceProfile/GetProfilePreviewText so a profile button's highlighted/tooltip state is readable without re-deriving it from raw settings. Upscaling runtimeUpgradeStatus reports device-query/context-confirmed runtime upgrade availability, provider identity, boot selection, last dispatched path and failure reason. Action-dispatched. listCommands/listQueries: params shortName, )"
		R"(returns [{name,description,executionThread}], where executionThread is main or render. Actions default to main; FEATURE_RENDER_COMMAND/FEATURE_RENDER_QUERY opt into the render thread that owns UI and preset state. invokeCommand: params shortName, name, args (object, optional) -- queued onto the registered owner thread, fire-and-forget. )"
		R"(invokeQuery: params shortName, name, args (object, optional) -- waits up to 5000ms for that owner thread and returns the result. Unknown shortName/name returns a plain error, never a crash. SceneManager queries presets and presetSceneSettings expose startup activation, Base Settings priority including disabled packs, and selected scene values; commands managePreset, copyPresetSettings and deletePresetSettings use the Presets page backend. Wind command setTreeBendEnabled takes mesh (string) and enabled (boolean), changing broad bending independently of leaf flutter.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"description": "List registered owner-thread metadata, queue a command, or wait for an owner-thread query.",
			"enum": ["listCommands","listQueries","invokeCommand","invokeQuery"]
		},
		"shortName": {"type":"string"},
		"name": {"type":"string","description":"Registered action name; Upscaling runtimeUpgradeStatus is a read-only query requiring no args."},
		"args": {"type":"object","properties":{"mesh":{"type":"string","description":"Mesh path for Wind setTreeBendEnabled."},"enabled":{"type":"boolean","description":"Wind bending state or the shared automatic-application switch for scene and Base Settings presets in SceneManager managePreset action=automatic."},"id":{"type":"string","description":"Preset identifier for SceneManager preset management."},"settings":{"type":"array","items":{"type":"string"},"description":"Stable setting keys for copyPresetSettings, or JSON pointer paths for deletePresetSettings."},"file":{"type":"string","description":"Relative preset payload path for deletePresetSettings."},"overwrite":{"type":"boolean","description":"copyPresetSettings replaces existing selected User Settings when true."},"replaceConflicts":{"type":"boolean","description":"managePreset action=apply may disable conflicting presets when true."},"entireFile":{"type":"boolean","description":"deletePresetSettings removes the backing file when true."}}}
	}
})"
		R"(})";
}
