#include "Utils/MathUtils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using Catch::Approx;

TEST_CASE("ClampFinite clamps in-range, low, and high inputs", "[utils][math]")
{
	REQUIRE(Util::ClampFinite(0.5f, 0.0f, 1.0f, 0.25f) == Approx(0.5f));
	REQUIRE(Util::ClampFinite(-1.0f, 0.0f, 1.0f, 0.25f) == Approx(0.0f));
	REQUIRE(Util::ClampFinite(2.0f, 0.0f, 1.0f, 0.25f) == Approx(1.0f));
	REQUIRE(Util::ClampFinite(64.0f, 64.0f, 4096.0f, 128.0f) == Approx(64.0f));
	REQUIRE(Util::ClampFinite(4096.0f, 64.0f, 4096.0f, 128.0f) == Approx(4096.0f));
}

TEST_CASE("ClampFinite falls back to default on non-finite input", "[utils][math]")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	REQUIRE(Util::ClampFinite(nan, 0.0f, 1.0f, 0.5f) == Approx(0.5f));
	REQUIRE(Util::ClampFinite(inf, 0.0f, 1.0f, 0.5f) == Approx(0.5f));
	REQUIRE(Util::ClampFinite(-inf, 0.0f, 1.0f, 0.5f) == Approx(0.5f));
}

TEST_CASE("ClampFiniteOrDefault behaves identically to ClampFinite", "[utils][math]")
{
	REQUIRE(Util::ClampFiniteOrDefault(0.75f, 0.0f, 1.0f, 0.1f) == Approx(0.75f));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	REQUIRE(Util::ClampFiniteOrDefault(nan, 0.0f, 1.0f, 0.1f) == Approx(0.1f));
}

TEST_CASE("QuantizeFloat rounds to nearest multiple of step", "[utils][math]")
{
	REQUIRE(Util::QuantizeFloat(1.23f, 0.5f) == Approx(1.0f));
	REQUIRE(Util::QuantizeFloat(1.26f, 0.5f) == Approx(1.5f));
	REQUIRE(Util::QuantizeFloat(10.0f, 2.5f) == Approx(10.0f));
}

TEST_CASE("HashCombine produces deterministic non-zero combinations", "[utils][math]")
{
	const auto h1 = Util::HashCombine(0, 42);
	const auto h2 = Util::HashCombine(h1, 100);
	REQUIRE(h1 != 0);
	REQUIRE(h2 != h1);
	REQUIRE(Util::HashCombine(0, 42) == h1);
}
