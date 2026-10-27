#include "Features/SnowCoverMath.h"
#include "Utils/HeightMap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE("Snow altitude reaches both seasonal peaks", "[snow-cover]")
{
	for (unsigned summer = 0; summer < 12; ++summer) {
		for (unsigned winter = 0; winter < 12; ++winter) {
			if (summer == winter) {
				REQUIRE(SnowCoverMath::SeasonalAltitude(5.5f, summer, winter, 18000.0f, -10000.0f) == Approx(-4000.0f));
				continue;
			}
			REQUIRE(SnowCoverMath::SeasonalAltitude(static_cast<float>(summer), summer, winter, 18000.0f, -10000.0f) == Approx(-18000.0f));
			REQUIRE(SnowCoverMath::SeasonalAltitude(static_cast<float>(winter), summer, winter, 18000.0f, -10000.0f) == Approx(10000.0f));
			const float beforeNewYear = SnowCoverMath::SeasonalAltitude(11.99999f, summer, winter, 18000.0f, -10000.0f);
			const float afterNewYear = SnowCoverMath::SeasonalAltitude(0.00001f, summer, winter, 18000.0f, -10000.0f);
			REQUIRE(beforeNewYear == Approx(afterNewYear).margin(1.0f));
		}
	}
}

TEST_CASE("Snow map bounds contain every filtered sample", "[snow-cover]")
{
	for (uint32_t width : { 1u, 2u, 7u, 16u, 33u }) {
		for (uint32_t height : { 1u, 3u, 16u, 31u }) {
			const size_t pitch = width + 3;
			std::vector<uint8_t> pixels(pitch * height, 0);
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x)
					pixels[y * pitch + x] = static_cast<uint8_t>((x * 17 + y * 31) % 256);
			Util::HeightMap map;
			REQUIRE(map.Load(width, height, pixels, pitch));
			for (unsigned start = 1; start < 8; ++start) {
				const float low = start / 16.0f;
				const float high = 1.0f - low;
				if (const auto maximum = map.Maximum(high, low, low, high)) {
					for (unsigned y = 0; y <= 16; ++y)
						for (unsigned x = 0; x <= 16; ++x)
							REQUIRE(map.Sample(std::lerp(low, high, x / 16.0f), std::lerp(low, high, y / 16.0f)) <= *maximum + 0.000001f);
				}
			}
		}
	}
}

TEST_CASE("Snow maps reject invalid input and exclude no unknown regions", "[snow-cover]")
{
	Util::HeightMap map;
	REQUIRE_FALSE(map.IsLoaded());
	const std::vector<uint8_t> pixels{ 0, 255, 255, 0 };
	REQUIRE_FALSE(map.Load(0, 2, pixels, 2));
	REQUIRE_FALSE(map.Load(2, 2, pixels, 1));
	REQUIRE_FALSE(map.Load(2, 3, pixels, 2));
	REQUIRE_FALSE(map.Maximum(0, 0, 1, 1));
	REQUIRE(map.Sample(0.5f, 0.5f) == 0.5f);
	REQUIRE(map.Load(2, 2, pixels, 2));
	REQUIRE(map.IsLoaded());
	REQUIRE(map.Sample(0.5f, 0.5f) == Approx(0.5f));
	REQUIRE(map.Sample(0.25f, 0.25f) == 0.0f);
	REQUIRE(map.Sample(0.75f, 0.25f) == 1.0f);
	REQUIRE_FALSE(map.Maximum(-1, 0, 0.5f, 0.5f));
	REQUIRE_FALSE(map.Maximum(0, 0, 1, 1));
}

TEST_CASE("Snow altitude follows the winter to summer arc across January", "[snow-cover]")
{
	REQUIRE(SnowCoverMath::SeasonalAltitude(0.0f, 3, 9, 20000.0f, -20000.0f) == Approx(0.0f));
	REQUIRE(SnowCoverMath::SeasonalAltitude(1.5f, 3, 9, 20000.0f, -20000.0f) == Approx(-10000.0f));
	REQUIRE(SnowCoverMath::SeasonalAltitude(10.5f, 3, 9, 20000.0f, -20000.0f) == Approx(10000.0f));
}

TEST_CASE("Snow accumulation handles rain and long waits", "[snow-cover]")
{
	using SnowCoverMath::Accumulate;
	REQUIRE(Accumulate(0.0f, 0.5f, true, false, 1.0f, 1.0f) == Approx(0.5f));
	REQUIRE(Accumulate(0.0f, 0.5f, false, true, 1.0f, 1.0f) == Approx(-1.0f));
	REQUIRE(Accumulate(0.25f, 1.0f, false, false, 1.0f, 1.0f) == 0.0f);
	REQUIRE(Accumulate(-0.25f, 1.0f, false, false, 1.0f, 1.0f) == 0.0f);
	REQUIRE(Accumulate(0.0f, 24.0f, true, false, 1.0f, 1.0f) == 2.0f);
	REQUIRE(Accumulate(0.0f, 24.0f, false, true, 1.0f, 1.0f) == -2.0f);
	REQUIRE(Accumulate(0.5f, 0.0f, false, true, 1.0f, 1.0f) == Approx(0.5f));
}
