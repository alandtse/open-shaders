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
	REQUIRE(Util::CalcFrameTime(1000, 1000) == Approx(1000.0f));
	REQUIRE(Util::CalcFrameTime(16, 1000) == Approx(16.0f));
	REQUIRE(Util::CalcFrameTime(0, 1000) == Approx(0.0f));
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
	REQUIRE(Util::Median({ 3.0f, 1.0f, 2.0f }) == Approx(2.0f));
	REQUIRE(Util::Median({ 4.0f, 1.0f, 3.0f, 2.0f }) == Approx(2.5f));
}

TEST_CASE("GetNowSecs is non-negative and non-decreasing", "[perfutils]")
{
	const double t0 = Util::GetNowSecs();
	const double t1 = Util::GetNowSecs();
	REQUIRE(t0 >= 0.0);
	REQUIRE(t1 >= t0);
}

TEST_CASE("QPC elapsed conversion preserves microsecond units", "[perfutils][qpc]")
{
	CHECK(Util::QpcElapsedUs(10'000, 15'000, 10'000'000) == Approx(500.0));
	CHECK(Util::QpcElapsedUs(10'000, 10'001, 10'000'000) == Approx(0.1));
	CHECK(Util::QpcElapsedUs(10'000, 10'000, 10'000'000) == 0.0);
	CHECK(Util::QpcElapsedUs(0, 15'000, 10'000'000) == 0.0);
	CHECK(Util::QpcElapsedUs(15'000, 10'000, 10'000'000) == 0.0);
	CHECK(Util::QpcElapsedUs(10'000, 15'000, 0) == 0.0);
}

TEST_CASE("QPC retry deadlines preserve 500 and 2000 ms and admit at the boundary", "[perfutils][qpc]")
{
	constexpr int64_t now = 100'000;
	constexpr int64_t frequency = 10'000'000;
	const auto first = Util::QpcDeadlineAfterMs(now, frequency, 500);
	const auto second = Util::QpcDeadlineAfterMs(now, frequency, 2000);
	CHECK(first == 5'100'000);
	CHECK(second == 20'100'000);
	CHECK(Util::QpcDeadlinePending(first - 1, first));
	CHECK_FALSE(Util::QpcDeadlinePending(first, first));
	CHECK_FALSE(Util::QpcDeadlinePending(first + 1, first));
	CHECK(Util::QpcDeadlinePending(second - 1, second));
	CHECK_FALSE(Util::QpcDeadlinePending(second, second));
	// Round up on a non-integral ticks-per-millisecond frequency.
	CHECK(Util::QpcDeadlineAfterMs(10, 1001, 500) == 511);
	CHECK(Util::QpcDeadlineAfterMs(now, frequency, 0) == now);
	CHECK(Util::QpcDeadlineAfterMs(now, 0, 500) == 0);
	CHECK(Util::QpcDeadlineAfterMs(0, frequency, 500) == 0);
	CHECK_FALSE(Util::QpcDeadlinePending(0, first));
	CHECK_FALSE(Util::QpcDeadlinePending(now, 0));
	const auto max = std::numeric_limits<int64_t>::max();
	CHECK(Util::QpcDeadlineAfterMs(max - 1, frequency, 500) == max);
}
