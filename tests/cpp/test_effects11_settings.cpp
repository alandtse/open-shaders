#include "Features/Effects11/SettingValueParser.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Effects11 accepts finite preset values", "[effects11]")
{
	float value = 0.0f;
	REQUIRE(Effects11Settings::TryParseFloat("  -1.25e2 \t", value));
	REQUIRE(value == -125.0f);
	REQUIRE(Effects11Settings::TryParseFloat("0", value));
	REQUIRE(value == 0.0f);
}

TEST_CASE("Effects11 accepts HLSL float literal suffixes", "[effects11]")
{
	float value = 0.0f;
	REQUIRE(Effects11Settings::TryParseHlslFloat("0.5f", value));
	REQUIRE(value == 0.5f);
	REQUIRE(Effects11Settings::TryParseHlslFloat(" 2.0F ", value));
	REQUIRE(value == 2.0f);
	REQUIRE(Effects11Settings::TryParseHlslFloat("3", value));
	REQUIRE(value == 3.0f);
}

TEST_CASE("Effects11 still rejects non-finite values with an HLSL suffix", "[effects11]")
{
	for (const auto* text : { "f", "inf", "nanf", "1e100f", "0.5ff" }) {
		CAPTURE(text);
		float value = 7.0f;
		REQUIRE_FALSE(Effects11Settings::TryParseHlslFloat(text, value));
		REQUIRE(value == 7.0f);
	}
}

TEST_CASE("Effects11 keeps defaults for malformed preset values", "[effects11]")
{
	for (const auto* text : { "", " ", "nan", "-nan", "inf", "-infinity", "1e100", "3.0junk", "2,3" }) {
		CAPTURE(text);
		float value = 7.0f;
		REQUIRE_FALSE(Effects11Settings::TryParseFloat(text, value));
		REQUIRE(value == 7.0f);
	}
}
