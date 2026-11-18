// Unit tests for Util::ShaderCompileFlags::SkipsOptimization (which shaders compile without FXC optimization).

#include "Utils/ShaderCompileFlags.h"

#include <catch2/catch_test_macros.hpp>

using Util::ShaderCompileFlags::SkipsOptimization;

TEST_CASE("SkipsOptimization: only Lighting pixel shaders on the default flag policy", "[shadercompileflags]")
{
	CHECK(SkipsOptimization(true, false, false, false));
	CHECK_FALSE(SkipsOptimization(false, false, false, false));
}

TEST_CASE("SkipsOptimization: debug, partial precision and avoid flow control keep their own policy", "[shadercompileflags]")
{
	CHECK_FALSE(SkipsOptimization(true, true, false, false));
	CHECK_FALSE(SkipsOptimization(true, false, true, false));
	CHECK_FALSE(SkipsOptimization(true, false, false, true));
	CHECK_FALSE(SkipsOptimization(true, true, true, true));
}
