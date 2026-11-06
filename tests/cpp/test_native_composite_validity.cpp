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
