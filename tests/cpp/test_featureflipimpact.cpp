// Unit tests for Util::FeatureFlipImpact (the compile-cost tier shown next to a feature's disable
// toggle). A missing or malformed table must read as "no estimate", never as a wrong tier.

#include "Utils/FeatureFlipImpact.h"

#include <catch2/catch_test_macros.hpp>

using namespace Util::FeatureFlipImpact;
using nlohmann::json;

namespace
{
	json Table()
	{
		return json::parse(R"({
			"schemaVersion": 1,
			"runtimes": {
				"SE": { "SSS": { "share": 0.043, "groups": 12 }, "SKYLIGHTING": { "share": 0.969, "groups": 1016 } },
				"VR": { "SSS": { "share": 0.05 } }
			}
		})");
	}
}

TEST_CASE("Tier boundaries sit at the documented shares", "[FeatureFlipImpact]")
{
	CHECK(TierFor(0.0) == Tier::Low);
	CHECK(TierFor(kLowBelow - 0.0001) == Tier::Low);
	CHECK(TierFor(kLowBelow) == Tier::Medium);
	CHECK(TierFor(kHighFrom - 0.0001) == Tier::Medium);
	CHECK(TierFor(kHighFrom) == Tier::High);
	CHECK(TierFor(1.0) == Tier::High);
}

TEST_CASE("Lookup returns the share and tier for the requested runtime", "[FeatureFlipImpact]")
{
	const auto low = Lookup(Table(), "SE", "SSS");
	REQUIRE(low.has_value());
	CHECK(low->share == 0.043);
	CHECK(low->tier == Tier::Low);

	const auto high = Lookup(Table(), "SE", "SKYLIGHTING");
	REQUIRE(high.has_value());
	CHECK(high->tier == Tier::High);

	const auto vr = Lookup(Table(), "VR", "SSS");
	REQUIRE(vr.has_value());
	CHECK(vr->share == 0.05);
}

TEST_CASE("Lookup is empty for anything the table does not describe", "[FeatureFlipImpact]")
{
	CHECK_FALSE(Lookup(Table(), "SE", "NO_SUCH_DEFINE").has_value());
	CHECK_FALSE(Lookup(Table(), "VR", "SKYLIGHTING").has_value());
	CHECK_FALSE(Lookup(Table(), "AE", "SSS").has_value());
	CHECK_FALSE(Lookup(Table(), "SE", "").has_value());
}

TEST_CASE("Lookup ignores a table it cannot trust", "[FeatureFlipImpact]")
{
	CHECK_FALSE(Lookup(json(), "SE", "SSS").has_value());
	CHECK_FALSE(Lookup(json::array(), "SE", "SSS").has_value());

	auto wrongVersion = Table();
	wrongVersion["schemaVersion"] = 2;
	CHECK_FALSE(Lookup(wrongVersion, "SE", "SSS").has_value());

	auto noRuntimes = Table();
	noRuntimes.erase("runtimes");
	CHECK_FALSE(Lookup(noRuntimes, "SE", "SSS").has_value());

	auto stringShare = Table();
	stringShare["runtimes"]["SE"]["SSS"]["share"] = "high";
	CHECK_FALSE(Lookup(stringShare, "SE", "SSS").has_value());

	auto entryNotObject = Table();
	entryNotObject["runtimes"]["SE"]["SSS"] = 0.2;
	CHECK_FALSE(Lookup(entryNotObject, "SE", "SSS").has_value());
}

TEST_CASE("Out-of-range shares are clamped rather than rejected", "[FeatureFlipImpact]")
{
	auto table = Table();
	table["runtimes"]["SE"]["SSS"]["share"] = 1.7;
	const auto above = Lookup(table, "SE", "SSS");
	REQUIRE(above.has_value());
	CHECK(above->share == 1.0);
	CHECK(above->tier == Tier::High);

	table["runtimes"]["SE"]["SSS"]["share"] = -0.2;
	const auto below = Lookup(table, "SE", "SSS");
	REQUIRE(below.has_value());
	CHECK(below->share == 0.0);
	CHECK(below->tier == Tier::Low);
}
