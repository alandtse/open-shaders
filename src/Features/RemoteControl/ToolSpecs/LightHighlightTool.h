#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.lighthighlight. {brand} is expanded at registration. */
	inline constexpr const char* kLightHighlightTool =
		R"({"description":")"
		R"(Forces one shadow-scheduler light to bright magenta (intensity 40) so a capture shows every pixel that light reaches. ptr is a light pointer string from inspect kind=llfshadows (slots[].ptr or visibility[].ptr); pass 'none' to clear. One light at a time; it persists until cleared.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"ptr": {"type":"string"}
	},
	"required": ["ptr"]
})"
		R"(})";
}
