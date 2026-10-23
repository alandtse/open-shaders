#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for inspect extension shadercache. {brand} is expanded at registration. */
	inline constexpr const char* kInspectShadercacheExt =
		R"({"description":")"
		R"({brand} shader-cache status -> {compiling,completedTasks,totalTasks,failedTasks,currentFailedCount,recentFailures,diskHitTasks,digestHitTasks,digestMissTasks,contentStoreHitTasks,digestComputeCount,)"
		R"(digestComputeTimeUs,frame_count,diskCacheHeld,featureSetChanged,featureSetRevertPending,featureSetCacheBackedUp,previousDiskCacheAvailable,cacheMismatches,)"
		R"(previousCacheMismatches}. Poll completedTasks against a pre-deploy snapshot to know a hot-reloaded shader finished; )"
		R"(watch failedTasks/currentFailedCount for failed compiles. recentFailures is the last 32 compile failures this session, each {key,path,error,epoch,frame} -- key is GetShaderString's cache key (shader class + merged feature #defines, identifying WHICH feature's shader broke without a separate lookup; )"
		R"(for ImageSpace shaders the filename embedded in key can differ from path), path is the actual source file that was compiled, )"
		R"(error is the compiler's own message (truncated to 2000 chars). digestHitTasks/digestMissTasks count disk-cache validity checks where the content-digest manifest matched (hit) or differed (miss); )"
		R"(the later blob load or recompilation can still fail; digestComputeTimeUs is the cumulative microseconds spent computing content digests this session; )"
		R"(contentDedupeTasks counts compiles skipped because an identical-bytecode task had already compiled the blob this session; contentStoreHitTasks counts compiles satisfied by the persistent content store; )"
		R"(activeReuseTasks and previousReuseTasks count blobs kept from the active or previous cache because their recorded content key matched the current code. diskCacheHeld true means a feature-set mismatch (see cacheMismatches, )"
		R"(each {kind,shortName,feature,detail,)"
		R"(nowPresent}) is holding the whole disk cache this session -- resolve with openshaders.shadercache acceptRebuild or restorePrevious. previousDiskCacheAvailable/previousCacheMismatches describe the rollback slot (the pre-change cache) a restorePrevious call would swap back in.")"
		R"(,"readOnly":true)"
		R"(,"inputSchema":{"type":"object"})"
		R"(})";
}
