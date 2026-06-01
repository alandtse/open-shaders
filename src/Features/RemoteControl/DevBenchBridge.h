#pragma once

// Client-side bridge to the devbench host (https://github.com/alandtse/devbench).
// Registers Open Shaders' tools into devbench over its cross-plugin C-ABI so they are
// drivable from the shared bench (MCP + REST).
//
// The implementation compiles only with -DDEVBENCH_BRIDGE_ENABLED (set by CMake when the
// `devbench-api` port is available); otherwise this file compiles to an empty Install().
// When built in, Install() is still a runtime no-op if no devbench host is present — so
// it is always safe to call.
namespace DevBenchBridge
{
	// Fetch the devbench interface (after kPostLoad) and register our tools. No-op if
	// devbench is not present or the bridge was built disabled. Safe to call always.
	void Install();
}
