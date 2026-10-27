#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.nvperf. {brand} is expanded at registration. */
	inline constexpr const char* kNvPerfTool =
		R"({"description":")"
		R"(NVIDIA Nsight Perf GPU hardware counters per render pass (D3D11; )"
		R"(builds with NVPERF_SUPPORT, )"
		R"(NVIDIA GPUs only). Ranges are the perf events {brand} emits (engine passes need Frame Annotations enabled at startup). Action-dispatched. status -> {built,available,gpuSupported,device,chip,)"
		R"(deviceError,library,capture:{state,captureId,sample,samples,pass,passes,error}}. metrics -> {defaults:[{metric,label}],)"
		R"(kinds}. capture: optional metrics (array of name[:counter|ratio|throughput|active|elapsed|percycle] strings, )"
		R"(default the defaults), ranges (array of names, trailing * = prefix; )"
		R"(default every outermost range), samples (1-64, default 1), delayFrames (default 0), nestingLevels (1-8, default 1; each level multiplies the passes) -> {queued,captureId}; )"
		R"(runs over the next frames, one profiler pass per frame. cancel: stop the running capture. result: optional captureId (default latest), )"
		R"(detail (boolean: include every range of every sample) -> {pending,capture} while running, )"
		R"(else {captureId,success,error,chip,startFrame,endFrame,samples,passesPerSample,metrics:[{metric,label}],unsupportedMetrics,summary:[{range,perFrame,incomplete,values:{label:value}}],csvPath}. )"
		R"(Summary values are per frame: counters summed (gpu__time_duration in ns), rates weighted by range time. Event openshaders.nvperf.completed {captureId,success,error} fires when a capture ends.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": ["status","metrics","capture","cancel","result"]
		},
		"metrics": {
			"type": "array",
			"items": {"type":"string"}
		},
		"ranges": {
			"type": "array",
			"items": {"type":"string"}
		},
		"samples": {"type":"number"},
		"delayFrames": {"type":"number"},
		"nestingLevels": {"type":"number"},
		"captureId": {"type":"number"},
		"detail": {"type":"boolean"}
	},
	"required": ["action"]
})"
		R"(})";
}
