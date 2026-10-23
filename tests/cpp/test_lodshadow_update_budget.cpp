#include "Features/LODShadowUpdateBudget.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("LOD shadow budget amortizes a delayed tiny batch", "[lod-shadows]")
{
	LODShadowUpdateBudget budget;
	for (int i = 0; i < 128; ++i)
		budget.Record(1000000, 0.1, 0.1);
	REQUIRE(budget.Triangles() == Catch::Approx(1000000).margin(1));
	budget.Record(3724, 0.982496, 0.1);
	REQUIRE(budget.Triangles() > 500000);
	REQUIRE(budget.Triangles() < 1000000);
}

TEST_CASE("LOD shadow budget follows sustained GPU cost changes", "[lod-shadows]")
{
	LODShadowUpdateBudget budget;
	for (int i = 0; i < 128; ++i)
		budget.Record(1000000, 0.1, 0.1);
	for (int i = 0; i < 16; ++i)
		budget.Record(1000000, 1.0, 0.1);
	REQUIRE(budget.Triangles() == Catch::Approx(100000).margin(1));
	for (int i = 0; i < 128; ++i)
		budget.Record(1000000, 0.1, 0.1);
	REQUIRE(budget.Triangles() == Catch::Approx(1000000).margin(1));
}

TEST_CASE("LOD shadow budget ignores unusable samples", "[lod-shadows]")
{
	LODShadowUpdateBudget budget;
	const auto initial = budget.Triangles();
	budget.Record(0, 0.1, 0.1);
	budget.Record(1000, 0, 0.1);
	budget.Record(1000, -1, 0.1);
	budget.Record(1000, std::numeric_limits<double>::infinity(), 0.1);
	budget.Record(1000, 0.1, std::numeric_limits<double>::quiet_NaN());
	REQUIRE(budget.Triangles() == initial);
}

TEST_CASE("LOD shadow budget bounds work and growth", "[lod-shadows]")
{
	LODShadowUpdateBudget budget;
	const auto initial = budget.Triangles();
	budget.Record(1000000, 0.01, 0.1);
	REQUIRE(budget.Triangles() <= uint32_t(initial * 1.1));
	for (int i = 0; i < 128; ++i)
		budget.Record(std::numeric_limits<uint32_t>::max(), 0.01, 0.1);
	REQUIRE(budget.Triangles() == 4194304);
	for (int i = 0; i < 16; ++i)
		budget.Record(1, 1.0, 0.1);
	REQUIRE(budget.Triangles() == 256);
}
