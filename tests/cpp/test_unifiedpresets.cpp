#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <fstream>
#include <winrt/base.h>

#include "Presets/PresetState.h"
#include "Utils/SettingsPatch.h"

TEST_CASE("Preset state retains metadata and ordered enabled layers", "[presets]")
{
	const json input{
		{ "activePackId", "Landscape" },
		{ "baselinePackIds", { "Foundation", "Landscape", "Foundation", "Night" } },
		{ "pipeline", "future-pipeline" },
		{ "pipelineRevision", 12 },
		{ "sceneControl", { { "Landscape", { { "futureSetting", true } } } } }
	};
	const auto result = PresetState::Normalize(input);
	REQUIRE(result["activePackId"] == "Landscape");
	REQUIRE(result["baselinePackIds"] == json::array({ "Foundation", "Landscape", "Night" }));
	REQUIRE(result["pipeline"] == input["pipeline"]);
	REQUIRE(result["pipelineRevision"] == input["pipelineRevision"]);
	REQUIRE(result["sceneControl"] == input["sceneControl"]);
	REQUIRE(PresetState::Normalize(result) == result);
}

TEST_CASE("Preset documents reject malformed, oversized and non-object data", "[presets]")
{
	const auto path = std::filesystem::temp_directory_path() /
	                  std::format("preset_document_{}_{}.json", ::GetCurrentProcessId(), std::chrono::steady_clock::now().time_since_epoch().count());
	struct RemoveDocument
	{
		std::filesystem::path path;
		~RemoveDocument()
		{
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	} cleanup{ path };
	const auto write = [&](std::string_view data) {
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << data;
		REQUIRE(output.good());
	};
	json document;
	REQUIRE_FALSE(PresetState::ReadDocument(path, document));
	for (const auto* data : { "", "{bad", "[]", "null", "42" }) {
		write(data);
		REQUIRE_FALSE(PresetState::ReadDocument(path, document));
	}
	write(R"({"name":"Portable","future":{"value":1}})");
	REQUIRE(PresetState::ReadDocument(path, document));
	REQUIRE(document["future"]["value"] == 1);
	write(std::string(PresetState::kMaxDocumentBytes + 1, ' '));
	REQUIRE_FALSE(PresetState::ReadDocument(path, document));
}

TEST_CASE("Primary preset files shadow complete fallback documents before parsing", "[presets]")
{
	const auto directory = std::filesystem::temp_directory_path() /
	                       std::format("preset_roots_{}_{}", ::GetCurrentProcessId(), std::chrono::steady_clock::now().time_since_epoch().count());
	struct RemoveDirectory
	{
		std::filesystem::path path;
		~RemoveDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	} cleanup{ directory };
	const std::array roots{ directory / "primary", directory / "fallback" };
	const std::filesystem::path relative = "Weather/Target/Pack_Settings.json";
	const auto write = [&](const std::filesystem::path& path, std::string_view data) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << data;
		REQUIRE(output.good());
	};
	write(roots[0] / relative, R"({"Value":1})");
	write(roots[1] / relative, R"({"Value":2,"FallbackOnly":3})");
	json document;
	const auto primary = PresetState::FindFirstExistingPayload(roots, relative);
	REQUIRE(primary == roots[0] / relative);
	REQUIRE(PresetState::ReadDocument(primary, document));
	REQUIRE(document == json{ { "Value", 1 } });

	write(primary, "{invalid");
	REQUIRE(PresetState::FindFirstExistingPayload(roots, relative) == primary);
	REQUIRE_FALSE(PresetState::ReadDocument(primary, document));
	std::filesystem::remove(primary);
	REQUIRE(PresetState::FindFirstExistingPayload(roots, relative) == roots[1] / relative);
	REQUIRE(PresetState::ReadDocument(roots[1] / relative, document));
	REQUIRE(document["FallbackOnly"] == 3);
	REQUIRE(PresetState::FindFirstExistingPayload(roots, "missing.json").empty());
	REQUIRE(PresetState::FindFirstExistingPayload(roots, "../outside.json").empty());
	REQUIRE(PresetState::FindFirstExistingPayload(roots, roots[0] / relative).empty());
}

TEST_CASE("Malformed preset state cannot escape the preset root", "[presets]")
{
	for (const json invalid : { json(nullptr), json(7), json("pack"), json::array() }) {
		const auto result = PresetState::Normalize(invalid);
		REQUIRE(result["activePackId"] == "");
		REQUIRE(result["baselinePackIds"] == json::array());
	}
	const auto normalized = PresetState::Normalize({ { "activePackId", "../outside" },
		{ "baselinePackIds", { "Good Pack", "../outside", ".", "_private", "C:\\outside", "a/b", "a\\b", 1, nullptr, "Good Pack" } } });
	REQUIRE(normalized["activePackId"] == "");
	REQUIRE(normalized["baselinePackIds"] == json::array({ "Good Pack" }));
}

TEST_CASE("Preset state retains opaque provider selections", "[presets]")
{
	for (const auto* id : { "provider:Imported", "legacy:Existing" })
		REQUIRE(PresetState::Normalize({ { "activePackId", id } })["activePackId"] == id);
	for (const auto* id : { ":", "provider:", ":pack", "provider:../outside", "provider:pack:extra", "bad/name:pack", "provider:COM1" })
		REQUIRE(PresetState::Normalize({ { "activePackId", id } })["activePackId"] == "");
}

TEST_CASE("Normalized feature edits are distinguished from legacy migration input", "[presets]")
{
	constexpr auto name = "Active Settings";
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(json::object(), name));
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(json(nullptr), name));
	for (const json version : { json(true), json("1"), json(PresetState::kFeatureSettingsVersion + 1) })
		REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits({ { PresetState::kFeatureSettingsVersionKey, version },
																 { PresetState::kNormalizedFeatureNamesKey, { name } } },
			name));
	const json state{ { PresetState::kFeatureSettingsVersionKey, PresetState::kFeatureSettingsVersion }, { "activePackId", "Portable" } };
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(PresetState::Normalize(state), name));
}

TEST_CASE("Saving normalized features retains inactive legacy migration status", "[presets]")
{
	json state{ { "activePackId", "Portable" }, { "futureMetadata", { { "value", 7 } } } };
	PresetState::MarkNormalizedFeatureEdits(state, "Active Settings");
	REQUIRE(PresetState::HasNormalizedFeatureEdits(state, "Active Settings"));
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(state, "Dormant Settings"));
	REQUIRE(state["futureMetadata"]["value"] == 7);

	auto nextSave = PresetState::Normalize(state);
	PresetState::MarkNormalizedFeatureEdits(nextSave, "Other Settings");
	PresetState::MarkNormalizedFeatureEdits(nextSave, "Other Settings");
	REQUIRE(PresetState::HasNormalizedFeatureEdits(nextSave, "Active Settings"));
	REQUIRE(PresetState::HasNormalizedFeatureEdits(nextSave, "Other Settings"));
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(nextSave, "Dormant Settings"));
	REQUIRE(nextSave[PresetState::kNormalizedFeatureNamesKey].size() == 2);

	PresetState::MarkNormalizedFeatureEdits(nextSave, "Dormant Settings");
	REQUIRE(PresetState::HasNormalizedFeatureEdits(nextSave, "Dormant Settings"));
}

TEST_CASE("Preset identifiers reject Windows device names and ambiguous components", "[presets]")
{
	for (const auto* id : { "CON", "nul.json", "COM1", "LPT9.json", "trailing.", "trailing ", "a\nb", "a\tb" })
		REQUIRE_FALSE(PresetState::IsPackId(id));
	REQUIRE(PresetState::IsPackId("Day and Night"));
	REQUIRE(PresetState::IsPackId("My.Preset"));
}

TEST_CASE("Baseline shape validation accepts partial trees and complete arrays", "[presets]")
{
	const json known{
		{ "Lighting", { { "Intensity", 1.0 }, { "Enabled", true } } },
		{ "Tint", { 1.0, 1.0, 1.0 } },
		{ "Asset", "default.dds" }
	};
	REQUIRE(PresetState::IsCompatibleBaseline({ { "Lighting", { { "Intensity", 2 } } } }, known));
	REQUIRE(PresetState::IsCompatibleBaseline({ { "Tint", { 1, 0.5, 0 } } }, known));
	REQUIRE(PresetState::IsCompatibleBaseline({ { "Asset", "preset.dds" } }, known));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Lighting", { { "Unknown", 1.0 } } } }, known));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Lighting", false } }, known));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Tint", { 1.0, 0.5 } } }, known));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Tint", { 1.0, "bad", 0.0 } } }, known));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Lighting", { { "Enabled", 1 } } } }, known));
}

TEST_CASE("Baseline numeric validation prevents unsafe integer conversions", "[presets]")
{
	const json unsignedKnown{ { "Count", std::uint32_t{ 1 } } };
	const json signedKnown{ { "Count", std::int32_t{ 1 } } };
	REQUIRE(PresetState::IsCompatibleBaseline({ { "Count", 2 } }, unsignedKnown));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Count", -1 } }, unsignedKnown));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Count", 1.5 } }, unsignedKnown));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Count", 1.5 } }, signedKnown));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Count", "2" } }, signedKnown));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Count", std::uint64_t{ 4294967296 } } }, unsignedKnown));
	REQUIRE_FALSE(PresetState::IsCompatibleBaseline({ { "Count", std::int64_t{ 2147483648 } } }, signedKnown));
}

TEST_CASE("Baseline switching retains personal deltas without serializing pack values", "[presets]")
{
	const json defaults{ { "Intensity", 1.0 }, { "Tint", { 1.0, 1.0, 1.0 } }, { "Enabled", true } };
	auto previousBase = defaults;
	previousBase.update({ { "Intensity", 2.0 } }, true);
	auto current = previousBase;
	current["Enabled"] = false;
	const auto personal = Util::Settings::BuildUserOverride(current, previousBase);
	REQUIRE(personal == json{ { "Enabled", false } });
	auto nextBase = defaults;
	nextBase.update({ { "Intensity", 3.0 }, { "Tint", { 0.2, 0.3, 0.4 } } }, true);
	auto resolved = nextBase;
	resolved.update(personal, true);
	REQUIRE(resolved["Intensity"] == 3.0);
	REQUIRE(resolved["Tint"] == nextBase["Tint"]);
	REQUIRE(resolved["Enabled"] == false);
	REQUIRE(Util::Settings::BuildUserOverride(resolved, nextBase) == personal);
}

TEST_CASE("Saving personal settings retains explicit values equal to the active Baseline", "[presets]")
{
	const json base{ { "Group", { { "Intensity", 2.0 }, { "Contrast", 1.0 } } }, { "Enabled", true } };
	const json savedKeys{ { "Group", { { "Intensity", 2.0 }, { "Removed", 7 } } } };
	const auto saved = Util::Settings::BuildUserOverride(base, base, savedKeys);
	REQUIRE(saved == json{ { "Group", { { "Intensity", 2.0 } } } });
	auto nextBase = base;
	nextBase["Group"]["Intensity"] = 3.0;
	nextBase["Group"]["Contrast"] = 1.5;
	auto resolved = nextBase;
	resolved.update(saved, true);
	REQUIRE(resolved["Group"]["Intensity"] == 2.0);
	REQUIRE(resolved["Group"]["Contrast"] == 1.5);
	REQUIRE(Util::Settings::BuildUserOverride(resolved, nextBase, saved) == saved);
}

TEST_CASE("Saving explicit boot preferences retains intent when the Baseline matches", "[presets]")
{
	const json base{ { "Optional", false }, { "Other", false } };
	const json preference{ { "Optional", false } };
	const auto saved = Util::Settings::BuildUserOverride(base, base, preference);
	REQUIRE(saved == preference);
	auto nextBase = base;
	nextBase["Optional"] = true;
	auto resolved = nextBase;
	resolved.update(saved, true);
	REQUIRE(resolved["Optional"] == false);
	REQUIRE_FALSE(saved.contains("Other"));
}

TEST_CASE("Climate period blending remains normalized through midnight and zero-width transitions", "[presets]")
{
	const std::array boundaries{ 5.0f, 7.5f, 10.0f, 16.0f, 18.5f, 21.0f };
	for (const auto width : { 0.0f, 1.0f, 2.0f }) {
		for (const auto ease : { 0.0f, 0.5f, 1.0f }) {
			for (int tick = 0; tick <= 96; ++tick) {
				const auto factors = PresetState::BlendPeriods(boundaries, width, ease, static_cast<float>(tick) * 0.25f);
				float sum = 0.0f;
				for (const auto factor : factors) {
					REQUIRE(std::isfinite(factor));
					REQUIRE(factor >= 0.0f);
					REQUIRE(factor <= 1.0f);
					sum += factor;
				}
				REQUIRE(sum == Catch::Approx(1.0f));
			}
		}
	}
	REQUIRE(PresetState::BlendPeriods(boundaries, 1.0f, 1.0f, 0.0f)[5] == 1.0f);
	REQUIRE(PresetState::BlendPeriods(boundaries, 1.0f, 1.0f, 12.0f)[2] == 1.0f);
	REQUIRE(PresetState::BlendPeriods(boundaries, 1.0f, 1.0f, 6.0f)[0] == 1.0f);
}

TEST_CASE("Climate period midpoints preview the selected phase", "[presets]")
{
	const std::array boundaries{ 5.0f, 7.5f, 10.0f, 16.0f, 18.5f, 21.0f };
	for (const auto width : { 0.0f, 1.0f, 2.0f }) {
		const auto blended = PresetState::BlendPeriodBoundaries(boundaries, width);
		for (size_t index = 0; index < boundaries.size(); ++index) {
			const auto end = index + 1 < boundaries.size() ? blended.starts[index + 1] : blended.starts.front() + 24.0f;
			const auto hour = std::fmod((blended.starts[index] + end) * 0.5f, 24.0f);
			const auto factors = PresetState::BlendPeriods(boundaries, width, 1.0f, hour);
			REQUIRE(factors[index] == Catch::Approx(1.0f));
		}
	}
}
