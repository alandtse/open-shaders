#include "Features/UnifiedWater/ShallowSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

namespace
{
	using Settings = UnifiedWaterSurface::Settings;
	constexpr std::array kFields{
		&Settings::ShallowFallbackStrength,
		&Settings::DeepConnectionProbeReachUnits,
		&Settings::DeepContextDepthUnits,
		&Settings::ShoreContactMinFadePixels,
		&Settings::ShoreDepthBlendRangeUnits,
		&Settings::ShallowSurfaceDepthRangeUnits,
		&Settings::ShallowFallbackMaxDistance,
		&Settings::DeepContextTransitionUnits
	};
}

TEST_CASE("Shallow water controls default on and occupy two shader registers", "[water][settings]")
{
	const Settings defaults;
	REQUIRE(defaults.ShallowFallbackStrength == 1.0f);
	REQUIRE_FALSE(defaults.UseOpenShadersDepthBehaviour);
	REQUIRE(UnifiedWaterSurface::ForRuntime(defaults, true).ShallowFallbackStrength == 1.0f);
	REQUIRE(UnifiedWaterSurface::ForRuntime(defaults, false).ShallowFallbackStrength == 0.0f);
	REQUIRE(sizeof(UnifiedWaterSurface::CommonBufferData) == 32);
	REQUIRE(alignof(UnifiedWaterSurface::CommonBufferData) == 16);
	REQUIRE(UnifiedWaterSurface::Sanitize(defaults) == defaults);
}

TEST_CASE("Shallow water controls clamp every finite input to the supported range", "[water][settings]")
{
	for (const auto field : kFields) {
		Settings input;
		input.*field = -1.0f;
		REQUIRE(UnifiedWaterSurface::Sanitize(input).*field == UnifiedWaterSurface::kMinimum.*field);
		input.*field = std::numeric_limits<float>::max();
		REQUIRE(UnifiedWaterSurface::Sanitize(input).*field == UnifiedWaterSurface::kMaximum.*field);
	}
}

TEST_CASE("Shallow water controls replace non-finite inputs with their own defaults", "[water][settings]")
{
	const Settings defaults;
	for (const auto field : kFields) {
		for (const float invalid : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() }) {
			Settings input;
			input.*field = invalid;
			REQUIRE(UnifiedWaterSurface::Sanitize(input) == defaults);
		}
	}
}

TEST_CASE("Missing water resources suppress the runtime strength without discarding the selection", "[water][settings]")
{
	Settings selected;
	selected.ShallowFallbackStrength = 0.8f;
	selected.DeepConnectionProbeReachUnits = 512.0f;
	const auto unavailable = UnifiedWaterSurface::ForRuntime(selected, false);
	REQUIRE(unavailable.ShallowFallbackStrength == 0.0f);
	REQUIRE(selected.ShallowFallbackStrength == 0.8f);
	for (const auto field : kFields) {
		if (field != &Settings::ShallowFallbackStrength)
			REQUIRE(unavailable.*field == selected.*field);
	}
	REQUIRE(UnifiedWaterSurface::ForRuntime(selected, true) == selected);
}

TEST_CASE("Legacy water depth blending suppresses restoration without changing selected controls", "[water][settings]")
{
	Settings selected;
	selected.ShallowFallbackStrength = 0.7f;
	selected.UseOpenShadersDepthBehaviour = true;
	REQUIRE(UnifiedWaterSurface::ForRuntime(selected, true).ShallowFallbackStrength == 0.0f);
	REQUIRE(selected.ShallowFallbackStrength == 0.7f);
	REQUIRE(selected.UseOpenShadersDepthBehaviour);
	const auto sanitized = UnifiedWaterSurface::Sanitize(selected);
	REQUIRE(sanitized == selected);
	selected.UseOpenShadersDepthBehaviour = false;
	REQUIRE(UnifiedWaterSurface::ForRuntime(selected, true).ShallowFallbackStrength == 0.7f);
}
