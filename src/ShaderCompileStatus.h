#pragma once

#include <cstdint>

// Minimal, dependency-free view of the shader (re)compile state for consumers
// that must NOT pull in ShaderCache.h — notably RemoteControl.cpp, where
// ShaderCache.h's global-scope `using namespace std::chrono` would leak chrono
// names (e.g. `last`) into the vendored cpp-mcp headers (httplib/base64) and
// trip warning-as-error. Defined in ShaderCache.cpp against the live cache.
namespace SIE
{
	struct ShaderCompileStatus
	{
		bool valid = false;  ///< false when the shader cache singleton is unavailable
		bool compiling = false;
		uint64_t completedTasks = 0;
		uint64_t totalTasks = 0;
		uint64_t failedTasks = 0;
		uint64_t currentFailedCount = 0;
	};

	/// Snapshot the current shader-cache compile counters. Thread-safe: reads
	/// atomics and takes the shader-map lock internally. Callable from the
	/// RemoteControl listener thread.
	ShaderCompileStatus GetShaderCompileStatus();
}
