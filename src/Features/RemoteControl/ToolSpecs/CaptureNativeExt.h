#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for capture extension openshaders.native. {brand} is expanded at registration. */
	inline constexpr const char* kCaptureNativeExt =
		R"({"description":")"
		R"(Native )"
		R"(SDR reference PNG from )"
		R"(Screenshot (key openshaders.native): same-cycle side-by-side eyes in VR, desktop in )"
		R"(SE/AE. Honors the host's absolute outputPath and requestId, never replaces files, )"
		R"(and emits capture.ready after verified publication or asynchronous preparation failure. Identical terminal retries replay capture.ready without recapture. UI exclusion is not guaranteed. Use the base capture tool's golden/threshold/regions or replay goldens for scoring; )"
		R"(temporal sequences use the openshaders provider's burst action.")"
		R"(,"inputSchema":{
	"type": "object",
	"required": ["outputPath","requestId"],
	"properties": {
		"outputPath": {"type":"string"},
		"requestId": {"type":"string","minLength":1,"maxLength":128}
	}
})"
		R"(})";
}
