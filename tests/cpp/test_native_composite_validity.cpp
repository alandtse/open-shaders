#include "../../features/Native Composite/Shaders/NativeComposite/ValidityPolicy.hlsli"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Native composite validity policies use initialized lighting only", "[native-composite]")
{
	using NativeCompositeValidity::AcceptContribution;
	constexpr uint32_t radiance = 0x20000000u;
	constexpr uint32_t forced = 0x80000000u;
	CHECK(AcceptContribution(0, 1, 1, radiance | 10));
	CHECK_FALSE(AcceptContribution(0, 0, 1, radiance | forced | 3));
	CHECK(AcceptContribution(1, 0, 1, radiance | forced | 3));
	CHECK_FALSE(AcceptContribution(1, 1, 2, radiance | forced | 10));
	CHECK(AcceptContribution(2, 1, 2, radiance | forced | 10));
	CHECK_FALSE(AcceptContribution(2, 0, 0, 2));
	CHECK_FALSE(AcceptContribution(3, 1, 1, radiance | 10));
}

TEST_CASE("Native-first readiness never upgrades incomplete pixels to full ownership", "[native-composite][native-pbr]")
{
	using NativeCompositeValidity::NativeFirstReady;
	constexpr uint32_t geometry = 0x08000000u;
	constexpr uint32_t material = 0x04000000u;
	constexpr uint32_t radiance = 0x20000000u;
	CHECK_FALSE(NativeFirstReady(0, 1, geometry | material | radiance, true));
	CHECK(NativeFirstReady(1, 0, geometry, false));
	CHECK_FALSE(NativeFirstReady(2, 0, geometry, false));
	CHECK(NativeFirstReady(2, 0, geometry | material, false));
	CHECK_FALSE(NativeFirstReady(3, 1, geometry | material, true));
	CHECK_FALSE(NativeFirstReady(3, 0, geometry | material | radiance, true));
	CHECK_FALSE(NativeFirstReady(3, 1, geometry | material | radiance, false));
	CHECK(NativeFirstReady(3, 1, geometry | material | radiance, true));
	CHECK_FALSE(NativeFirstReady(3, 1, material | radiance, true));
}
