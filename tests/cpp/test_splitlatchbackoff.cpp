// Unit tests for SplitLatchBackoff, the split-cache latch retry backoff.

#include "Features/LightLimitFix/SplitLatchBackoff.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using SplitLatchBackoff::NextRetryFrames;
using SplitLatchBackoff::RetryDue;

TEST_CASE("A retry falls due when the backoff interval has elapsed", "[SplitLatchBackoff]")
{
	CHECK_FALSE(RetryDue(599u, 0u, 600u));
	CHECK(RetryDue(600u, 0u, 600u));
	CHECK(RetryDue(601u, 0u, 600u));
}

TEST_CASE("A frame counter below the latch frame makes the retry due", "[SplitLatchBackoff]")
{
	CHECK(RetryDue(0u, 500u, 600u));
	CHECK(RetryDue(499u, 500u, 600u));
}

TEST_CASE("Each retry doubles the backoff", "[SplitLatchBackoff]")
{
	CHECK(NextRetryFrames(SplitLatchBackoff::kInitialRetryFrames) == 1200u);
	CHECK(NextRetryFrames(1200u) == 2400u);
	CHECK(NextRetryFrames(2400u) == 4800u);
}

TEST_CASE("The backoff clamps at the maximum", "[SplitLatchBackoff]")
{
	CHECK(NextRetryFrames(4800u) == SplitLatchBackoff::kMaxRetryFrames);
	CHECK(NextRetryFrames(SplitLatchBackoff::kMaxRetryFrames) == SplitLatchBackoff::kMaxRetryFrames);
}

TEST_CASE("A huge backoff clamps instead of overflowing", "[SplitLatchBackoff]")
{
	CHECK(NextRetryFrames(UINT32_MAX) == SplitLatchBackoff::kMaxRetryFrames);
	CHECK(NextRetryFrames(UINT32_MAX / 2u + 1u) == SplitLatchBackoff::kMaxRetryFrames);
}
