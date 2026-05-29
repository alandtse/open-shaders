// Unit tests for ShadowCasterManager pure helpers
// (src/Features/LightLimitFix/ShadowCasterMath.h): the shadow-light pointer
// plausibility check and the frame-time 90th-percentile.

#include "Features/LightLimitFix/ShadowCasterMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using ShadowCasterManager::FrameTimePercentile90;
using ShadowCasterManager::IsPlausibleShadowLightPtr;

TEST_CASE("IsPlausibleShadowLightPtr rejects null, misaligned, and non-canonical", "[scm]")
{
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0));                      // null
	REQUIRE(IsPlausibleShadowLightPtr(0x8));                         // aligned, in range
	REQUIRE(IsPlausibleShadowLightPtr(0x00007FFFFFFFFFF8ull));       // top of user-mode range
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x0000800000000000ull)); // first non-canonical
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0xFFFFF80000000000ull)); // kernel-space garbage

	// Any non-8-byte alignment is rejected.
	for (std::uintptr_t off = 1; off < 8; ++off)
		REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x1000 + off));
	REQUIRE(IsPlausibleShadowLightPtr(0x1000));
}

TEST_CASE("FrameTimePercentile90 returns the 60fps fallback with no samples", "[scm]")
{
	float ring[8]{};
	REQUIRE(FrameTimePercentile90(ring, 0) == Approx(16.67f));
}

TEST_CASE("FrameTimePercentile90 picks the P90 element", "[scm]")
{
	// 10 samples 1..10: idx = int(10*0.9) = 9 -> the largest sorted element.
	float ring[10] = { 5, 2, 9, 1, 7, 3, 10, 4, 8, 6 };
	REQUIRE(FrameTimePercentile90(ring, 10) == Approx(10.0f));
}

TEST_CASE("FrameTimePercentile90 honors count below the window size", "[scm]")
{
	// Only the first 5 entries are valid; trailing slots must not be sampled.
	float ring[10] = { 10, 20, 30, 40, 50, 999, 999, 999, 999, 999 };
	// n = 5, idx = int(5*0.9) = 4 -> the largest of {10..50} = 50.
	REQUIRE(FrameTimePercentile90(ring, 5) == Approx(50.0f));
}
