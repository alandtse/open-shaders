#include "Features/DX12Resources/AutoCellResolver.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Automatic DX12 CELL detection resolves unique save EditorID first", "[dx12_resources]")
{
	using namespace DX12ResourceService;
	const std::vector<InteriorCellCandidate> cells{
		{ { "Skyrim.esm", 0x101 }, "BluePalace01", "Blue Palace" },
		{ { "MyMod.esp", 0x202 }, "MyModRoom", "Blue Palace" }
	};
	SaveLocationHint save;
	save.editorId = "BluePalace01";
	save.locationName = "Blue Palace";
	auto r = ResolveSaveCell(save, cells);
	REQUIRE(r.cell);
	REQUIRE(r.cell->plugin == "Skyrim.esm");
	REQUIRE(r.cell->localId == 0x101);
	save.editorId = "Unknown";
	r = ResolveSaveCell(save, cells);
	REQUIRE_FALSE(r.cell);
	REQUIRE(r.candidates == 2);
	save.locationName = "Missing";
	r = ResolveSaveCell(save, cells);
	REQUIRE_FALSE(r.cell);
}

TEST_CASE("DX12 Resources uses unambiguous localized CELL name only as fallback", "[dx12_resources]")
{
	using namespace DX12ResourceService;
	SaveLocationHint hint;
	hint.editorId = "UnknownEditor";
	hint.locationName = "The Bannered Mare";
	const std::vector<InteriorCellCandidate> cells{
		{ { "Skyrim.esm", 0x1122 }, "WhiterunBanneredMare", "The Bannered Mare" },
		{ { "Skyrim.esm", 0x1200 }, "SolitudeBluePalace", "Blue Palace" }
	};
	const auto matched = ResolveSaveCell(hint, cells);
	REQUIRE(matched.cell);
	REQUIRE(matched.cell->localId == 0x1122);
	auto ambiguous = cells;
	ambiguous.push_back({ { "Example.esp", 0x100 }, "ExampleTavern", "The Bannered Mare" });
	const auto refused = ResolveSaveCell(hint, ambiguous);
	REQUIRE_FALSE(refused.cell);
	REQUIRE(refused.candidates == 2);
}
