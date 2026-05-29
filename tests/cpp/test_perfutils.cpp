// Unit tests for Util perf/stat helpers (src/Utils/PerfUtils.h).
//
// Header-only, pure logic; GetNowSecs wraps QueryPerformanceCounter but is
// self-contained. No game/D3D/ImGui dependency.

#include <Windows.h>  // LARGE_INTEGER / QueryPerformance* used by PerfUtils.h

#include "Utils/PerfUtils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using Catch::Approx;

TEST_CASE("CalcFrameTime converts ticks to milliseconds", "[perfutils]")
{
	// 1000 ticks at 1000 Hz == 1.0 s == 1000 ms.
	REQUIRE(Util::CalcFrameTime(1000, 1000) == Approx(1000.0f));
	// 16 ticks at 1000 Hz == 16 ms.
	REQUIRE(Util::CalcFrameTime(16, 1000) == Approx(16.0f));
	// Zero elapsed is zero regardless of frequency.
	REQUIRE(Util::CalcFrameTime(0, 1000) == Approx(0.0f));
	// Scales inversely with frequency.
	REQUIRE(Util::CalcFrameTime(1000, 2000) == Approx(500.0f));
}

TEST_CASE("CalcFPS guards non-positive frame time", "[perfutils]")
{
	REQUIRE(Util::CalcFPS(0.0f) == 0.0f);
	REQUIRE(Util::CalcFPS(-5.0f) == 0.0f);
	REQUIRE(Util::CalcFPS(1000.0f) == Approx(1.0f));
	REQUIRE(Util::CalcFPS(1000.0f / 60.0f) == Approx(60.0f));
}

TEST_CASE("Mean handles empty, single, and multi-element vectors", "[perfutils]")
{
	REQUIRE(Util::Mean({}) == 0.0f);
	REQUIRE(Util::Mean({ 5.0f }) == Approx(5.0f));
	REQUIRE(Util::Mean({ 2.0f, 4.0f, 6.0f }) == Approx(4.0f));
	REQUIRE(Util::Mean({ -1.0f, 1.0f }) == Approx(0.0f));
}

TEST_CASE("Median handles empty, odd, even, and unsorted input", "[perfutils]")
{
	REQUIRE(Util::Median({}) == 0.0f);
	REQUIRE(Util::Median({ 7.0f }) == Approx(7.0f));
	// Odd: middle of sorted {1,2,3} is 2.
	REQUIRE(Util::Median({ 3.0f, 1.0f, 2.0f }) == Approx(2.0f));
	// Even: average of two middles of sorted {1,2,3,4} is 2.5.
	REQUIRE(Util::Median({ 4.0f, 1.0f, 3.0f, 2.0f }) == Approx(2.5f));
}

TEST_CASE("GetNowSecs is non-negative and non-decreasing", "[perfutils]")
{
	const double t0 = Util::GetNowSecs();
	const double t1 = Util::GetNowSecs();
	REQUIRE(t0 >= 0.0);
	REQUIRE(t1 >= t0);
}
