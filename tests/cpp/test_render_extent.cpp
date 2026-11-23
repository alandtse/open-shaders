// Unit tests for the render-extent helpers that keep feature setup valid while the window is minimized.

#include "Utils/RenderExtent.h"

#include <catch2/catch_test_macros.hpp>

using Util::IsMinimizedExtent;
using Util::MipChainLength;

TEST_CASE("The engine's 8x8 minimized target is detected and real targets are not", "[RenderExtent]")
{
	CHECK(IsMinimizedExtent(8, 8));
	CHECK(IsMinimizedExtent(0, 0));
	CHECK(IsMinimizedExtent(1920, 8));
	CHECK(IsMinimizedExtent(8, 1080));
	CHECK_FALSE(IsMinimizedExtent(16, 16));
	CHECK_FALSE(IsMinimizedExtent(1280, 720));
	CHECK_FALSE(IsMinimizedExtent(2560, 1440));
}

TEST_CASE("A mip chain never exceeds what D3D11 allows for the largest side", "[RenderExtent]")
{
	CHECK(MipChainLength(2560, 1440, 5) == 5);
	CHECK(MipChainLength(16, 16, 5) == 5);
	CHECK(MipChainLength(15, 15, 5) == 4);
	CHECK(MipChainLength(8, 8, 5) == 4);
	CHECK(MipChainLength(1, 1, 5) == 1);
	CHECK(MipChainLength(0, 0, 5) == 1);
	CHECK(MipChainLength(8, 1024, 5) == 5);
	CHECK(MipChainLength(1024, 1024, 3) == 3);
}
