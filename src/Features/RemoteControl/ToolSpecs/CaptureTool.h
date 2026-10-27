#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.capture. {brand} is expanded at registration. */
	inline constexpr const char* kCaptureTool =
		R"({"description":")"
		R"(Trigger a frame capture on the next render. Kind-dispatched. kind=renderdoc: RenderDoc multi-frame capture via the in-app API, honors frames (1-120, default 1); RenderDoc must be attached/loaded (check openshaders.feature list for RenderDoc.loaded). kind=screenshot: lossless screenshot via the )"
		R"(Screenshot feature; )"
		R"(frames is ignored. kind=shadowmaps: writes the shadow atlas depth texture (DDS) + slot-manifest JSON to Data/SKSE/Plugins/CommunityShaders/Captures on the next shadow pass (atlas mode only): ground truth for tile contents without a RenderDoc attach. With kind=shadowmaps and frames=N it records N shadow frames,)"
		R"( )"
		R"(and slot (an integer light index, default -1 for none) adds that light's caster set. Fire-and-forget: no artifact path is returned synchronously.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"kind": {
			"type": "string",
			"enum": ["renderdoc","screenshot","shadowmaps"]
		},
		"frames": {"type":"number"},
		"slot": {"type":"integer"}
	},
	"required": ["kind"]
})"
		R"(})";
}
