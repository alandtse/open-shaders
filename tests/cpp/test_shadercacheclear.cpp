// Unit tests for the pure scope-resolution logic behind the scene-scoped
// ("smart") shader cache clear. Shift-click must always give the opposite of
// the configured default, in both directions - this is the one piece of that
// feature's UI logic that doesn't need a live ImGui/game context to verify.

#include "Utils/ShaderCacheClearScope.h"

#include <catch2/catch_test_macros.hpp>

using Util::ResolveShaderCacheClearScope;
using Util::ShaderCacheClearScope;

TEST_CASE("ResolveShaderCacheClearScope: default false", "[shadercacheclear]")
{
	CHECK(ResolveShaderCacheClearScope(false, false) == ShaderCacheClearScope::Full);
	CHECK(ResolveShaderCacheClearScope(false, true) == ShaderCacheClearScope::ActiveOnly);
}

TEST_CASE("ResolveShaderCacheClearScope: default true", "[shadercacheclear]")
{
	CHECK(ResolveShaderCacheClearScope(true, false) == ShaderCacheClearScope::ActiveOnly);
	CHECK(ResolveShaderCacheClearScope(true, true) == ShaderCacheClearScope::Full);
}
