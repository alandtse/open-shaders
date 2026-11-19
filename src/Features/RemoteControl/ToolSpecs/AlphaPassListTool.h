#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for the session-only native alpha-queue diagnostic. */
	inline constexpr const char* kAlphaPassListTool = R"({
	"description": "Inspect {brand}' VR linked alpha-pass protection. status (default) reports installed, enabled, active, installationError, duplicatesSuppressed, cacheAllocationFailures, unverifiableMemberships, per-thread cache limits and generation. set requires a boolean enabled for a session-only A/B comparison; true requires successful hook installation. Protection defaults on each launch. Disabling permits the original queue corruption and is for controlled testing only. Runs on the render thread; SE/AE report inactive. Counters are cumulative for this launch.",
	"inputSchema": {
		"type": "object",
		"properties": {
			"action": { "type": "string", "enum": ["status", "set"], "default": "status" },
			"enabled": { "type": "boolean" }
		},
		"additionalProperties": false,
		"oneOf": [
			{ "properties": { "action": { "const": "status" } }, "not": { "required": ["enabled"] } },
			{ "properties": { "action": { "const": "set" } }, "required": ["action", "enabled"] }
		]
	}
})";
}
