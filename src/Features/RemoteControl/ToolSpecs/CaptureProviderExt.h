#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for capture extension openshaders. {brand} is expanded at registration. */
	inline constexpr const char* kCaptureProviderExt =
		R"({"description":")"
		R"({brand} lossless screenshot as a devbench capture provider (key openshaders), so `capture`, `record replay` checkpoints and goldens work without the low-fidelity native fallback. The )"
		R"(Screenshot feature writes a PNG to the outputPath devbench supplies on the next present and answers with a capture.ready event (width, )"
		R"(height, renderFrame). renderFrame is the engine graphics frame copied on the render thread, not the request frame. subrect is an optional {x,y,w,h} in 0..1 UV of the capture source. The source is the final VR composite on VR (both eyes unless subrect crops one) and the framebuffer on flat, )"
		R"(both of which include the HUD. Only flat HDR can draw without the HUD, so excludeUi (default true) is honored there alone; )"
		R"(every other capture still includes the HUD and the event reports uiExcluded false, which devbench marks degraded. One request at a time; )"
		R"(an active native frame sequence rejects still requests. The crop preset, clipboard copy and HUD notice of a user screenshot do not apply. Frame-locked burst: action=burst with requestId, )"
		R"(outputDir (absolute), prefix, count (1..600), everyNFrames (1..600, default 1) and optional manifestPath runs the native frame-sequence engine and answers {queued,runId}; )"
		R"(it saves whole native )"
		R"(SDR PNG frames (subrect and scale are refused; )"
		R"(VR saves both eyes side by side) as <prefix>_NNNN.png in outputDir, then writes the manifest {requestId,width,height,format,uiExcluded,everyNFrames,frames:[{index,file,gameFrame,gameTimeMs,wallMs}],dropped:[{index,gameFrame}]} (default <prefix>_manifest.json; )"
		R"(gameTimeMs equals wallMs) and emits capture.burst.progress {requestId,captured,dropped} while it runs and capture.burst.ready {requestId,ok,error?,captured,dropped,width,height,uiExcluded,)"
		R"(manifestPath} at the end. A frame the writer cannot keep up with is dropped and counted. action=burstStatus with runId answers {active,done,requested,saved,failed,dropped,)"
		R"(manifest}. On a flat HDR display the frames come from the HDR Display feature's clean-scene )"
		R"(SDR output, so they have no HUD or menu and uiExcluded is true.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": ["capture","burst","burstStatus"]
		},
		"requestId": {"type":"string","maxLength":128},
		"outputPath": {"type":"string"},
		"outputDir": {"type":"string"},
		"prefix": {"type":"string"},
		"manifestPath": {"type":"string"},
		"count": {"type":"integer","minimum":1,"maximum":600},
		"everyNFrames": {"type":"integer","minimum":1,"maximum":600},
		"excludeUi": {"type":"boolean"},
		"runId": {"type":"integer"},
		"subrect": {"type":"object"}
	}
})"
		R"(})";
}
