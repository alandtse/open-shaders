// Unit tests for the ray helpers Interior Sun's opening scan uses.

#include "Features/InteriorSun/WindowRays.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace InteriorSunRays;

namespace
{
	// One triangle in the z = 10 plane covering the origin's column
	const std::vector<float3> wall = { { -5.0f, -5.0f, 10.0f }, { 5.0f, -5.0f, 10.0f }, { 0.0f, 5.0f, 10.0f } };
}

TEST_CASE("A ray toward a triangle hits it at its distance", "[interiorsun]")
{
	REQUIRE(CastRay({ 0, 0, 0 }, { 0, 0, 1 }, 100.0f, wall, 0, 3) == Catch::Approx(10.0f));
}

TEST_CASE("A ray misses a triangle beside it, behind it or out of reach", "[interiorsun]")
{
	CHECK(CastRay({ 20, 0, 0 }, { 0, 0, 1 }, 100.0f, wall, 0, 3) == FLT_MAX);
	CHECK(CastRay({ 0, 0, 0 }, { 0, 0, -1 }, 100.0f, wall, 0, 3) == FLT_MAX);
	CHECK(CastRay({ 0, 0, 0 }, { 0, 0, 1 }, 5.0f, wall, 0, 3) == FLT_MAX);
}

TEST_CASE("The nearest of several triangles is returned", "[interiorsun]")
{
	auto both = wall;
	for (const auto& v : wall)
		both.push_back({ v.x, v.y, 4.0f });
	REQUIRE(CastRay({ 0, 0, 0 }, { 0, 0, 1 }, 100.0f, both, 0, 6) == Catch::Approx(4.0f));
}

TEST_CASE("A ray hits a box only when it passes through it within reach", "[interiorsun]")
{
	const float3 lo(-1, -1, 9), hi(1, 1, 11);
	CHECK(RayHitsBox({ 0, 0, 0 }, { 0, 0, 1 }, 100.0f, lo, hi));
	CHECK_FALSE(RayHitsBox({ 5, 0, 0 }, { 0, 0, 1 }, 100.0f, lo, hi));
	CHECK_FALSE(RayHitsBox({ 0, 0, 0 }, { 0, 0, 1 }, 5.0f, lo, hi));
	CHECK_FALSE(RayHitsBox({ 0, 0, 0 }, { 0, 0, -1 }, 100.0f, lo, hi));
}
