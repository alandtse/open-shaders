#include "../../features/Native Lighting/Shaders/NativeLighting/ShadowCull.hlsli"
#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("Early shadow rejection requires authoritative matching evidence", "[native-lighting]")
{
	using NativeLighting::CanCullShadow;
	CHECK(CanCullShadow(true, true, 0, 1, 10, 1));
	CHECK_FALSE(CanCullShadow(false, true, 0, 1, 10, 1));
	CHECK_FALSE(CanCullShadow(true, false, 0, 1, 10, 1));
	CHECK_FALSE(CanCullShadow(true, true, 0.01f, 1, 10, 1));
	CHECK_FALSE(CanCullShadow(true, true, 0, -1, 10, 1));
	CHECK_FALSE(CanCullShadow(true, true, 0, 0, 10, 1));
	CHECK_FALSE(CanCullShadow(true, true, 0, 1, 0, 1));
	CHECK_FALSE(CanCullShadow(true, true, 0, 1, 10, 0));
	CHECK_FALSE(CanCullShadow(true, true, std::numeric_limits<float>::quiet_NaN(), 1, 10, 1));
	CHECK_FALSE(CanCullShadow(true, true, 0, 1, std::numeric_limits<float>::infinity(), 1));
	CHECK_FALSE(CanCullShadow(true, true, 0, 1, 10, std::numeric_limits<float>::quiet_NaN()));
}
