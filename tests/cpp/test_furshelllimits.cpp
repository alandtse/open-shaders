#include "Features/FurShells/ShellLimits.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
	struct Values
	{
		uint32_t ShellCount = 12;
		float Length = 1.2f;
		float BodyLength = 0.4f;
		float Droop = 0.15f;
		float RootThreshold = 0.0f;
		float TipThreshold = 1.0f;
		float RootDarkening = 0.6f;
		float ShellColor = 1.0f;
		float FadeStart = 700.0f;
		float FadeEnd = 2500.0f;
		float MinFurPixels = 0.0f;
		float WindStrength = 1.0f;
		float WetStrength = 1.0f;
		float TouchStrength = 1.0f;
	};
}

TEST_CASE("Hand-edited fur settings are brought into range", "[fur-shells]")
{
	Values low{ 0, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
	FurShellLimits::ClampSettings(low, 32);
	REQUIRE(low.ShellCount == 1);
	REQUIRE(low.Length == 0.05f);
	REQUIRE(low.BodyLength == 0.05f);
	REQUIRE(low.FadeEnd == 0.0f);
	REQUIRE(low.TouchStrength == 0.0f);

	Values high{ 1000, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 1e6f, 1e6f, 100.0f, 100.0f, 100.0f, 100.0f };
	FurShellLimits::ClampSettings(high, 32);
	REQUIRE(high.ShellCount == 32);
	REQUIRE(high.Length == 6.0f);
	REQUIRE(high.BodyLength == 3.0f);
	REQUIRE(high.MinFurPixels == 16.0f);
	REQUIRE(high.FadeStart == 8000.0f);
	REQUIRE(high.WetStrength == 1.0f);
}

TEST_CASE("Default fur settings pass through unchanged", "[fur-shells]")
{
	Values values;
	FurShellLimits::ClampSettings(values, 32);
	REQUIRE(values.ShellCount == 12);
	REQUIRE(values.Length == 1.2f);
	REQUIRE(values.FadeEnd == 2500.0f);
	REQUIRE(values.TouchStrength == 1.0f);
}
