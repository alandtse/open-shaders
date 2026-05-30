// Unit tests for the LightLimitFix settings sanitizer
// (src/Features/LightLimitFix/SettingsSanitize.h).

#include "Features/LightLimitFix/SettingsSanitize.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using Catch::Approx;
using LightLimitFixSanitize::SanitizeFloat;

TEST_CASE("SanitizeFloat clamps in-range, low, and high inputs", "[llf]")
{
	REQUIRE(SanitizeFloat(0.5f, 0.0f, 1.0f) == Approx(0.5f));
	REQUIRE(SanitizeFloat(-1.0f, 0.0f, 1.0f) == Approx(0.0f));
	REQUIRE(SanitizeFloat(2.0f, 0.0f, 1.0f) == Approx(1.0f));
	REQUIRE(SanitizeFloat(64.0f, 64.0f, 4096.0f) == Approx(64.0f));      // exact lower bound
	REQUIRE(SanitizeFloat(4096.0f, 64.0f, 4096.0f) == Approx(4096.0f));  // exact upper bound
}

TEST_CASE("SanitizeFloat falls back to the lower bound on non-finite input", "[llf]")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	REQUIRE(SanitizeFloat(nan, 0.5f, 8.0f) == Approx(0.5f));
	REQUIRE(SanitizeFloat(inf, 0.5f, 8.0f) == Approx(0.5f));
	REQUIRE(SanitizeFloat(-inf, 0.5f, 8.0f) == Approx(0.5f));
}
