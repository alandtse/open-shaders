#include <catch2/catch_test_macros.hpp>

#include "Features/PerformanceOverlay/ABTesting/PrerequisiteChain.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	struct FakeSetting
	{
		std::string key;
		std::string enabledByKey;
		std::string enabledByPath;
		std::int8_t enabledByValue = -1;
	};

	using Catalog = std::map<std::string, FakeSetting>;
	using Chain = std::vector<std::pair<std::string, int>>;

	FakeSetting& Add(Catalog& catalog, const std::string& key)
	{
		auto& setting = catalog[key];
		setting.key = key;
		return setting;
	}

	void Require(FakeSetting& setting, const std::string& prerequisite, std::int8_t value)
	{
		setting.enabledByKey = prerequisite;
		setting.enabledByValue = value;
	}

	std::optional<Chain> Resolve(const FakeSetting& setting, const Catalog& catalog,
		const std::map<std::string, int>& current = {})
	{
		const auto chain = ABPrerequisites::ResolvePrerequisites<FakeSetting, int>(setting, [&catalog](std::string_view key, std::string_view) -> const FakeSetting* {
				const auto match = catalog.find(std::string(key));
				return match == catalog.end() ? nullptr : &match->second; }, [&current](const FakeSetting& prerequisite) {
				const auto match = current.find(prerequisite.key);
				return match == current.end() ? 0 : match->second; }, [](const FakeSetting&, std::int8_t required) { return static_cast<int>(required); });
		if (!chain)
			return std::nullopt;
		Chain steps;
		for (const auto& step : *chain)
			steps.emplace_back(step.setting->key, step.required);
		return steps;
	}
}

TEST_CASE("A one-level chain lists the setting that disables it", "[prerequisitechain]")
{
	Catalog catalog;
	Require(Add(catalog, "subject"), "enable", 1);
	Add(catalog, "enable");

	const auto chain = Resolve(catalog.at("subject"), catalog, { { "enable", 0 } });
	REQUIRE(chain);
	REQUIRE(chain->size() == 1);
	CHECK(chain->front().first == "enable");
	CHECK(chain->front().second == 1);
}

TEST_CASE("A chain is ordered outermost prerequisite first", "[prerequisitechain]")
{
	Catalog catalog;
	Require(Add(catalog, "subject"), "middle", 1);
	Require(Add(catalog, "middle"), "outer", 1);
	Add(catalog, "outer");

	const auto chain = Resolve(catalog.at("subject"), catalog, { { "middle", 0 }, { "outer", 0 } });
	REQUIRE(chain);
	REQUIRE(chain->size() == 2);
	CHECK(chain->at(0).first == "outer");
	CHECK(chain->at(1).first == "middle");
}

TEST_CASE("A prerequisite already at its value is skipped", "[prerequisitechain]")
{
	Catalog catalog;
	Require(Add(catalog, "subject"), "middle", 1);
	Require(Add(catalog, "middle"), "outer", 1);
	Add(catalog, "outer");

	const auto satisfied = Resolve(catalog.at("subject"), catalog, { { "middle", 1 }, { "outer", 1 } });
	REQUIRE(satisfied);
	CHECK(satisfied->empty());

	const auto partlySatisfied = Resolve(catalog.at("subject"), catalog, { { "middle", 1 }, { "outer", 0 } });
	REQUIRE(partlySatisfied);
	REQUIRE(partlySatisfied->size() == 1);
	CHECK(partlySatisfied->front().first == "outer");
}

TEST_CASE("A prerequisite that must be off is listed when it is on", "[prerequisitechain]")
{
	Catalog catalog;
	Require(Add(catalog, "subject"), "prerequisite", 0);
	Add(catalog, "prerequisite");

	const auto chain = Resolve(catalog.at("subject"), catalog, { { "prerequisite", 1 } });
	REQUIRE(chain);
	REQUIRE(chain->size() == 1);
	CHECK(chain->front().first == "prerequisite");
	CHECK(chain->front().second == 0);
}

TEST_CASE("A cycle fails to resolve", "[prerequisitechain]")
{
	Catalog catalog;
	Require(Add(catalog, "subject"), "first", 1);
	Require(Add(catalog, "first"), "second", 1);
	Require(Add(catalog, "second"), "first", 1);

	CHECK_FALSE(Resolve(catalog.at("subject"), catalog));
}

TEST_CASE("A setting that is not catalogued fails to resolve", "[prerequisitechain]")
{
	Catalog catalog;
	Require(Add(catalog, "subject"), "missing", 1);

	CHECK_FALSE(Resolve(catalog.at("subject"), catalog));
}

TEST_CASE("A chain deeper than the limit fails to resolve", "[prerequisitechain]")
{
	Catalog catalog;
	Add(catalog, "subject");
	std::string previous = "subject";
	for (int level = 0; level < static_cast<int>(ABPrerequisites::kMaxDepth); ++level) {
		const auto name = "level" + std::to_string(level);
		Add(catalog, name);
		Require(catalog.at(previous), name, 1);
		previous = name;
	}

	const auto chain = Resolve(catalog.at("subject"), catalog);
	REQUIRE(chain);
	CHECK(chain->size() == ABPrerequisites::kMaxDepth);

	Add(catalog, "deeper");
	Require(catalog.at(previous), "deeper", 1);
	CHECK_FALSE(Resolve(catalog.at("subject"), catalog));
}
