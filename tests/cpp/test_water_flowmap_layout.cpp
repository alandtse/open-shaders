#include "Features/UnifiedWater/FlowmapLayout.h"
#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("Water flowmap cell dimensions are bounded", "[water][flowmap]")
{
	using namespace WaterFlowmapLayout;
	REQUIRE(ValidCells(1, 1));
	REQUIRE(ValidCells(MaxCells, MaxCells));
	REQUIRE_FALSE(ValidCells(0, 1));
	REQUIRE_FALSE(ValidCells(1, -1));
	REQUIRE_FALSE(ValidCells(MaxCells + 1, 1));
	REQUIRE_FALSE(ValidCells(std::numeric_limits<std::int32_t>::max(), 1));
}

TEST_CASE("Water flowmap accepts matching mip reductions only", "[water][flowmap]")
{
	using namespace WaterFlowmapLayout;
	for (std::uint32_t mip = 0; mip < 6; ++mip)
		REQUIRE(MatchesTexture(256u >> mip, 128u >> mip, 4, 2));
	REQUIRE_FALSE(MatchesTexture(128, 128, 4, 2));
	REQUIRE_FALSE(MatchesTexture(4, 2, 4, 2));
	REQUIRE_FALSE(MatchesTexture(0, 0, 0, 0));
	REQUIRE_FALSE(MatchesTexture(64, 64, -1, 1));
}
