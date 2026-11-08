// Unit tests for the WeatherIDs= tokenizer (src/Features/Effects11/WeatherIDParser.cpp).
// Pure string parsing; no game/D3D dependency.

#include "CSEditor/Weather/PrecipitationSettings.h"
#include "Features/Effects11/WeatherIDParser.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Precipitation box sizes accept upstream integers and legacy float bit views", "[precipitation]")
{
	REQUIRE(PrecipitationSerialization::ReadBoxSize(2000.0) == 2000u);
	REQUIRE(PrecipitationSerialization::ReadBoxSize(2.802596928649634e-42) == 2000u);
	REQUIRE(PrecipitationSerialization::ReadBoxSize(1.401298464324817e-45) == 1u);
	REQUIRE(PrecipitationSerialization::ReadBoxSize(4294967295.0) == 4294967295u);
	for (const double invalid : { 0.0, -1.0, 0.5, 4294967296.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
		REQUIRE_FALSE(PrecipitationSerialization::ReadBoxSize(invalid));
}

TEST_CASE("Parse accepts comma-separated IDs", "[weatheridparser]")
{
	// Real format used by e.g. the stock ENB `_weatherlist.ini` on SE.
	auto result = WeatherIDParser::Parse("10e1e6, 10e1e8, 10a234, 10a230");

	REQUIRE(result.weatherIDs == std::vector<uint32_t>{ 0x10e1e6, 0x10e1e8, 0x10a234, 0x10a230 });
	REQUIRE(result.invalidTokens.empty());
}

TEST_CASE("Parse accepts whitespace-separated IDs with no commas", "[weatheridparser]")
{
	// Real format used by some VR ENB presets ("Scenery ENB VR - LUX Edition") --
	// the regression this parser was written to fix: without whitespace
	// splitting, only the first ID here would ever be read.
	auto result = WeatherIDParser::Parse("17D613 17D614 17D615 179A75");

	REQUIRE(result.weatherIDs == std::vector<uint32_t>{ 0x17D613, 0x17D614, 0x17D615, 0x179A75 });
	REQUIRE(result.invalidTokens.empty());
}

TEST_CASE("Parse accepts a mix of commas and whitespace", "[weatheridparser]")
{
	auto result = WeatherIDParser::Parse("10a233,10a236 , 10a245  10e1ed");

	REQUIRE(result.weatherIDs == std::vector<uint32_t>{ 0x10a233, 0x10a236, 0x10a245, 0x10e1ed });
	REQUIRE(result.invalidTokens.empty());
}

TEST_CASE("Parse returns nothing for an empty value", "[weatheridparser]")
{
	auto result = WeatherIDParser::Parse("");

	REQUIRE(result.weatherIDs.empty());
	REQUIRE(result.invalidTokens.empty());
}

TEST_CASE("Parse skips a malformed token and reports it as invalid", "[weatheridparser]")
{
	auto result = WeatherIDParser::Parse("10e1e6, not-hex, 10a234");

	REQUIRE(result.weatherIDs == std::vector<uint32_t>{ 0x10e1e6, 0x10a234 });
	REQUIRE(result.invalidTokens.size() == 1);
	REQUIRE(result.invalidTokens[0].first == "not-hex");
}

TEST_CASE("ParseHexID parses a bare hex string and treats empty as zero", "[weatheridparser]")
{
	REQUIRE(WeatherIDParser::ParseHexID("10a234") == 0x10a234);
	REQUIRE(WeatherIDParser::ParseHexID("") == 0);
}
