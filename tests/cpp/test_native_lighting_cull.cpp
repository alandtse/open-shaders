#include "../../features/Native Lighting/Shaders/NativeLighting/ShadowCull.hlsli"
#include "../../features/Native Lighting/Shaders/NativeLighting/ValidityLab.hlsli"
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

TEST_CASE("Validity Lab tolerances and experimental mask remain bounded", "[native-lighting]")
{
	using NativeLighting::DepthMatches;
	using NativeLighting::CanCullExperimentalMask;
	CHECK(DepthMatches(true, 2.0f, 100, 2.0f, 0.00001f));
	CHECK_FALSE(DepthMatches(true, 3.0f, 100, 2.0f, 0.00001f));
	CHECK(DepthMatches(true, 3.0f, 100, 4.0f, 0.00001f));
	CHECK_FALSE(DepthMatches(false, 0, 100, 4, 0));
	CHECK_FALSE(DepthMatches(true, std::numeric_limits<float>::infinity(), 100, 4, 0));
	CHECK(CanCullExperimentalMask(true, true, true, 0.1f, 1, 10, 1, 0.2f));
	CHECK_FALSE(CanCullExperimentalMask(false, true, true, 0, 1, 10, 1, 0.2f));
	CHECK_FALSE(CanCullExperimentalMask(true, false, true, 0, 1, 10, 1, 0.2f));
	CHECK_FALSE(CanCullExperimentalMask(true, true, false, 0, 1, 10, 1, 0.2f));
	CHECK_FALSE(CanCullExperimentalMask(true, true, true, -1, 1, 10, 1, 0.2f));
	CHECK_FALSE(CanCullExperimentalMask(true, true, true, 0, 1, 10, 1, std::numeric_limits<float>::quiet_NaN()));
}
