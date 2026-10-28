#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.profiler. {brand} is expanded at registration. */
	inline constexpr const char* kProfilerTool =
		R"({"description":")"
		R"(Enable or disable {brand}' runtime GPU/CPU profiler capture. Action-dispatched. enable/disable: )"
		R"(SetUserEnabled(true/false) -- the same flag the in-game Profiling page's checkbox drives, so this is the one state transition (off->on) a script can reach no other way. Read timing data via inspect kind=profiler; )"
		R"(that tool also requests a capture on its own, so a script doesn't need to enable AND separately request one, only enable if disabled. perDrawZones toggles the high-volume per-draw Tracy zones (State::Draw and its nested zones, per-feature OnRenderPassBegin) at runtime, default off; calls/frame stay visible as the zonecalls.* Tracy plots either way.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": ["enable","disable","perDrawZones"]
		},
		"enabled": {
			"type": "boolean"
		}
	},
	"required": ["action"]
})"
		R"(})";
}
