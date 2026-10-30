#include "Features/RainRefraction/TextureFootprint.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("Rain normal map padding preserves physical drop width", "[rain]")
{
	std::vector<uint8_t> pixels(20 * 20 * 4);
	for (size_t y = 6; y < 14; ++y) {
		for (size_t x = 8; x < 12; ++x)
			pixels[(y * 20 + x) * 4 + 3] = 205;
	}
	const auto footprint = RainRefractionTexture::GetFootprint(pixels, 20, 20, 80);
	REQUIRE(footprint.uvBounds == std::array<float, 4>{ 0.35f, 0.25f, 0.65f, 0.75f });
	REQUIRE(footprint.radiusScale == std::array<float, 2>{ 1.5f, 2.5f });
}

TEST_CASE("Rain normal map borders clamp to the image", "[rain]")
{
	std::vector<uint8_t> pixels(32 * 32 * 4, 255);
	const auto footprint = RainRefractionTexture::GetFootprint(pixels, 32, 32, 128);
	REQUIRE(footprint.uvBounds == std::array<float, 4>{ 0, 0, 1, 1 });
	REQUIRE(footprint.radiusScale == std::array<float, 2>{ 1, 1 });
}

TEST_CASE("Rain normal map row padding is not alpha coverage", "[rain]")
{
	std::vector<uint8_t> pixels(16 * 4, 255);
	for (size_t y = 0; y < 4; ++y) {
		for (size_t x = 0; x < 2; ++x)
			pixels[y * 16 + x * 4 + 3] = 0;
	}
	const auto footprint = RainRefractionTexture::GetFootprint(pixels, 2, 4, 16);
	REQUIRE(footprint.uvBounds == std::array<float, 4>{ 0, 0, 1, 1 });
	REQUIRE(footprint.radiusScale == std::array<float, 2>{ 1, 1 });
	REQUIRE(RainRefractionTexture::GetFootprint(pixels, 5, 4, 16).radiusScale == footprint.radiusScale);
}
