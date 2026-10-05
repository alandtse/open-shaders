// Unit tests for Util::FrameLatch, which keeps the wetness rain clock and retained character
// wetness advancing once per rendered frame regardless of how many times buffers are built.

#include "Utils/FrameLatch.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using Util::FrameLatch;

TEST_CASE("The first call for a frame advances and repeats do not", "[FrameLatch]")
{
	FrameLatch latch;
	CHECK(latch.TryAdvance(10));
	CHECK_FALSE(latch.TryAdvance(10));
	CHECK_FALSE(latch.TryAdvance(10));
}

TEST_CASE("Each new frame advances once", "[FrameLatch]")
{
	FrameLatch latch;
	int advances = 0;
	for (std::uint32_t frame = 0; frame < 5; ++frame)
		for (int call = 0; call < 4; ++call)
			advances += latch.TryAdvance(frame) ? 1 : 0;
	CHECK(advances == 5);
}

TEST_CASE("A call that cannot advance leaves the frame open", "[FrameLatch]")
{
	FrameLatch latch;
	CHECK_FALSE(latch.TryAdvance(7, false));
	CHECK(latch.TryAdvance(7, true));
	CHECK_FALSE(latch.TryAdvance(7, true));
}

TEST_CASE("Frame zero advances on a fresh latch", "[FrameLatch]")
{
	FrameLatch latch;
	CHECK(latch.TryAdvance(0));
}

TEST_CASE("The largest frame value advances like any other", "[FrameLatch]")
{
	FrameLatch latch;
	CHECK(latch.TryAdvance(UINT32_MAX));
	CHECK_FALSE(latch.TryAdvance(UINT32_MAX));
}

TEST_CASE("A frame counter that wraps advances again", "[FrameLatch]")
{
	FrameLatch latch;
	CHECK(latch.TryAdvance(UINT32_MAX));
	CHECK(latch.TryAdvance(0));
	CHECK_FALSE(latch.TryAdvance(0));
}

TEST_CASE("Reset lets the same frame advance again", "[FrameLatch]")
{
	FrameLatch latch;
	CHECK(latch.TryAdvance(3));
	latch.Reset();
	CHECK(latch.TryAdvance(3));
}
