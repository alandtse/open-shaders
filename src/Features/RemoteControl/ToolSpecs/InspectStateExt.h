#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for inspect extension openshaders. {brand} is expanded at registration. */
	inline constexpr const char* kInspectStateExt =
		R"({"description":")"
		R"({brand} engine state -> {plugin,frame_count,vr}. frame_count increases each render tick — use it as ground truth that a queued operation has had a frame to run.")"
		R"(,"readOnly":true)"
		R"(,"inputSchema":{"type":"object"})"
		R"(})";
}
