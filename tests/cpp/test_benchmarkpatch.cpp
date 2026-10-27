// Unit tests for the minimal merge patch the settings benchmark builds from a single changed setting.

#include "Features/PerformanceOverlay/ABTesting/SettingsPatch.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
	using nlohmann::json;
	using SettingsBenchmark::MakePatch;
}

TEST_CASE("Benchmark patch is empty when nothing differs", "[settingsbenchmark][patch]")
{
	const json settings = { { "a", 1 }, { "nested", { { "b", true } } } };
	CHECK(MakePatch(settings, settings) == json::object());
}

TEST_CASE("Benchmark patch keeps only the changed nested leaf", "[settingsbenchmark][patch]")
{
	const json original = { { "a", 1 }, { "nested", { { "b", true }, { "c", 2.5 } } }, { "other", "x" } };
	auto modified = original;
	modified["nested"]["b"] = false;

	CHECK(MakePatch(original, modified) == json{ { "nested", { { "b", false } } } });
}

TEST_CASE("Benchmark patch copies a changed array whole", "[settingsbenchmark][patch]")
{
	const json original = { { "color", { 1.0, 0.5, 0.25 } }, { "scale", 2 } };
	auto modified = original;
	modified["color"][1] = 0.75;

	CHECK(MakePatch(original, modified) == json{ { "color", { 1.0, 0.75, 0.25 } } });
}

TEST_CASE("Benchmark patch carries keys that are new and never removes old ones", "[settingsbenchmark][patch]")
{
	const json original = { { "kept", 1 }, { "dropped", 2 } };
	const json modified = { { "kept", 1 }, { "added", 3 } };

	CHECK(MakePatch(original, modified) == json{ { "added", 3 } });
}

TEST_CASE("Benchmark patch merges back to the modified settings", "[settingsbenchmark][patch]")
{
	const json original = { { "a", 1 }, { "nested", { { "b", 1 }, { "list", { 1, 2, 3 } } } } };
	auto modified = original;
	modified["nested"]["b"] = 9;
	modified["nested"]["list"][2] = 7;

	auto merged = original;
	merged.merge_patch(MakePatch(original, modified));
	CHECK(merged == modified);
}
