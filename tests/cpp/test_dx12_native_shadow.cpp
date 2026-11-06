#include "../../features/DX12 Native Scene/Shaders/DX12NativeScene/NativeShadowMask.hlsli"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Native shadow visibility preserves unavailable receivers", "[dx12-native-scene]")
{
	using namespace DX12Native;
	CHECK_FALSE(NativeShadowValid(false, true, 1));
	CHECK_FALSE(NativeShadowValid(true, false, 1));
	CHECK_FALSE(NativeShadowValid(true, true, 0));
	CHECK_FALSE(NativeShadowValid(true, true, 60001));
	CHECK(NativeShadowLightVisibility(false, 1.0f, false) == 0.0f);
	CHECK(NativeShadowLightVisibility(true, 1.0f, false) == 1.0f);
	CHECK(NativeShadowSampleState(false) == -1.0f);
	CHECK(NativeShadowSampleState(true) == 1.0f);
}

TEST_CASE("Native shadow visibility weights contributing lights", "[dx12-native-scene]")
{
	using namespace DX12Native;
	const auto first = NativeShadowLightWeight(2.0f, 0.5f, 1.0f);
	const auto second = NativeShadowLightWeight(1.0f, 0.5f, 1.0f);
	CHECK(NativeShadowWeightedVisibility(first, first + second) == Catch::Approx(2.0f / 3.0f));
	CHECK(NativeShadowWeightedVisibility(0.0f, 0.0f) == 1.0f);
	CHECK(NativeShadowClip(0, 2) == Catch::Approx(-0.5f));
	CHECK(NativeShadowClip(1, 2) == Catch::Approx(0.5f));
}
