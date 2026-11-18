// Layout tests for the LightLimitFix common-buffer struct
// (src/Features/LightLimitFix/PerFrame.h), which mirrors
// SharedData::LightLimitFixSettings in the shared feature cbuffer.

#include "Features/LightLimitFix/PerFrame.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

using LightLimitFixCB::PerFrame;

TEST_CASE("LLF PerFrame keeps its 64-byte, 16-aligned footprint", "[llf]")
{
	STATIC_REQUIRE(sizeof(PerFrame) == 64);
	STATIC_REQUIRE(alignof(PerFrame) == 16);
}

TEST_CASE("LLF PerFrame ClusteredLightCount occupies the former trailing pad slot", "[llf]")
{
	STATIC_REQUIRE(offsetof(PerFrame, ClusteredLightCount) == 60);
	STATIC_REQUIRE(offsetof(PerFrame, ClusteredLightCount) + sizeof(std::uint32_t) == sizeof(PerFrame));
}

TEST_CASE("LLF PerFrame cluster config stays on a 16-byte boundary", "[llf]")
{
	STATIC_REQUIRE(offsetof(PerFrame, ClusterSize) % 16 == 0);
	STATIC_REQUIRE(offsetof(PerFrame, ClusterSize) == 32);
	STATIC_REQUIRE(offsetof(PerFrame, EnableLightsVisualisation) == 48);
}

TEST_CASE("LLF PerFrame value-initializes ClusteredLightCount to zero", "[llf]")
{
	const PerFrame perFrame{};
	CHECK(perFrame.ClusteredLightCount == 0);
}
