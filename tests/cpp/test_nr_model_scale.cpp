// Unit tests for the Neural Rendering model scale: the size the model runs at, and how the tuning bounds the scale.
// These exercise the production helpers only.

#include "Features/Upscaling/NeuralRendering/ModelScale.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("ModelExtent runs the model at the eye size for a full or unusable scale", "[nr]")
{
	const std::pair<uint32_t, uint32_t> none{ 0, 0 };
	REQUIRE(NR::ModelExtent(1.0f, 1080, 1200) == none);
	REQUIRE(NR::ModelExtent(2.0f, 1080, 1200) == none);
	REQUIRE(NR::ModelExtent(std::numeric_limits<float>::quiet_NaN(), 1080, 1200) == none);
	REQUIRE(NR::ModelExtent(std::numeric_limits<float>::infinity(), 1080, 1200) == none);
}

TEST_CASE("ModelExtent rounds the reduced size to the kernel group", "[nr]")
{
	const auto half = NR::ModelExtent(0.5f, 1080, 1200);
	REQUIRE(half == std::pair<uint32_t, uint32_t>{ 544, 600 });
	const auto threeQuarters = NR::ModelExtent(0.75f, 1080, 1200);
	REQUIRE(threeQuarters == std::pair<uint32_t, uint32_t>{ 808, 904 });
	REQUIRE(half.first % NR::kModelExtentAlignment == 0);
	REQUIRE(half.second % NR::kModelExtentAlignment == 0);
}

TEST_CASE("ModelExtent never goes below the minimum scale or the minimum side", "[nr]")
{
	REQUIRE(NR::ModelExtent(0.01f, 1080, 1200) == NR::ModelExtent(NR::Tuning::kMinModelScale, 1080, 1200));
	const auto tiny = NR::ModelExtent(0.5f, 80, 80);
	REQUIRE(tiny.first == NR::kMinModelExtent);
	REQUIRE(tiny.second == NR::kMinModelExtent);
}

TEST_CASE("ModelExtent runs the model at the eye size when the reduced size would not be smaller", "[nr]")
{
	const std::pair<uint32_t, uint32_t> none{ 0, 0 };
	REQUIRE(NR::ModelExtent(0.5f, NR::kMinModelExtent, 1200) == none);
	REQUIRE(NR::ModelExtent(0.5f, 1080, NR::kMinModelExtent) == none);
}

TEST_CASE("Sanitize bounds the model scale to its range", "[nr]")
{
	NR::Tuning tuning;
	tuning.modelScale = 5.0f;
	tuning.Sanitize();
	REQUIRE(tuning.modelScale == NR::Tuning::kMaxModelScale);
	tuning.modelScale = 0.0f;
	tuning.Sanitize();
	REQUIRE(tuning.modelScale == NR::Tuning::kMinModelScale);
	tuning.modelScale = std::numeric_limits<float>::quiet_NaN();
	tuning.Sanitize();
	REQUIRE(tuning.modelScale == NR::Tuning::kMaxModelScale);
	tuning.modelScale = 0.6f;
	tuning.Sanitize();
	REQUIRE(tuning.modelScale == 0.6f);
}
