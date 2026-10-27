#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for openshaders.shadercache. {brand} is expanded at registration. */
	inline constexpr const char* kShadercacheTool =
		R"({"description":")"
		R"(Manage {brand}' compiled shader cache. clear, deleteDisk, activeOnly, acceptRebuild, restorePrevious, and discardPrevious are queued onto the main thread, )"
		R"(fire-and-forget. backgroundCompile and contentStore are immediate atomic state changes made on the calling (devbench listener) thread. clear: drop the IN-MEMORY cache only; )"
		R"(with the disk cache enabled shaders reload from Data/ShaderCache rather than recompiling, so this does NOT guarantee a recompile. deleteDisk: delete the on-disk cache AND drop the in-memory cache, )"
		R"(forcing a full cold recompile (use this for compile benchmarks). activeOnly: the in-game 'smart clear' -- captures whatever shaders are on screen over two windows, )"
		R"(then evicts+recompiles just those (needs something rendering; )"
		R"(a menu-only screen may capture nothing). backgroundCompile: skip the boot-time wait for the FULL eager compile queue to drain -- same effect as the in-game 'Skip Compilation' hotkey. Compilation keeps running in the background afterward (fewer threads, )"
		R"(so it doesn't starve gameplay), but the game becomes playable/scriptable immediately. For a benchmark harness: call this once right after launch, )"
		R"(then drive one throwaway replay to demand-compile just that scene's own shaders before the timed run, )"
		R"(instead of waiting out every permutation the whole install could ever need (can be 20-30 minutes on a large AIO modlist). acceptRebuild: when a feature-set change is holding the disk cache (see inspect(kind=shadercache).diskCacheHeld/cacheMismatches), )"
		R"(accept it and rebuild for the current setup -- mirrors the in-game prompt's 'rebuild' button. restorePrevious: swap the rollback slot (the pre-change cache) back into the active slot -- mirrors the in-game prompt's 'restore previous' button; )"
		R"(cancels an in-progress compile if one is running, only takes effect after a restart, )"
		R"(check inspect(kind=shadercache).featureSetRevertPending or the log for the outcome. discardPrevious: delete the rollback slot so the current cache is kept and the home-page mismatch notice clears -- mirrors the in-game 'keep current cache' button; )"
		R"(refused while a feature-set change awaits commit (featureSetChanged), a restore is pending, or the cache is held. contentStore (param enabled: bool): toggle the developer/tester Advanced setting that persists compiled blobs keyed by the preprocessed source, entry, profile, flags and compiler; )"
		R"(the store lives in Data/ShaderCache/ContentStore and survives invalidation, rotation and file-watcher wipes, )"
		R"(but deleteDisk removes it too. Watch progress via inspect kind=shadercache and the openshaders.shaderRecompiled event. Read-only status (including rollback/backup-slot state) is inspect kind=shadercache. exportTrace: write every task record from the current build to a Chrome Trace Event Format JSON file (importable at ui.perfetto.dev or chrome://tracing) -- a timeline can localize external CPU contention during a build in a way aggregate stats can't. Runs synchronously on the calling thread (read-only over the record set plus a file write). Optional 'path' overrides the default (next to OpenShaders.log); )"
		R"(fails if the current build has no recorded tasks yet.")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"action": {
			"type": "string",
			"enum": [
				"clear",
				"deleteDisk",
				"activeOnly",
				"backgroundCompile",
				"contentStore",
				"acceptRebuild",
				"restorePrevious",
				"discardPrevious",
				"exportTrace"
			]
		},
		"enabled": {
			"type": "boolean",
			"description": "contentStore only: turn the content-addressed compile store on or off."
		},
		"path": {
			"type": "string",
			"description": "exportTrace only: destination file path; defaults to OpenShaders.log's directory."
		}
	},
	"required": ["action"]
})"
		R"(})";
}
