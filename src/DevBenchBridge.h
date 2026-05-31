#pragma once

// Client-side bridge to the devbench host (https://github.com/alandtse/devbench).
// Registers Community Shaders' tools into devbench over its cross-plugin C-ABI so
// devbench can supersede CS's built-in RemoteControl MCP server.
//
// INERT by default: the implementation compiles only with -DDEVBENCH_BRIDGE_ENABLED
// (which is set when the `devbench-api` vcpkg port is available — see DevBenchBridge.cpp
// for activation steps). Until then Install() is a no-op, so CS builds unchanged even
// though CONFIGURE_DEPENDS auto-globs this .cpp.
namespace DevBenchBridge
{
	// Fetch the devbench interface (after kPostLoad) and register CS's tools. No-op if
	// devbench is not installed or the bridge is built disabled. Safe to call always.
	void Install();
}
