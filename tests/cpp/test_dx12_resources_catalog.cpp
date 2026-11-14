#include "Features/DX12Resources/PluginCellCatalog.h"
#include <array>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("DX12 Resources distinguishes owning-plugin interiors and CELL overrides", "[dx12_resources]")
{
	using namespace DX12ResourceService;
	const std::array<std::string_view, 2> sources{ "Skyrim.esm", "Unslaad.esm" };
	REQUIRE(ClassifyPluginCell("skyrim.ESM", "Skyrim.esm", sources) == PluginCellRelation::Defined);
	REQUIRE(ClassifyPluginCell("UNSLAAD.esm", "Skyrim.esm", sources) == PluginCellRelation::Override);
	REQUIRE(ClassifyPluginCell("Glenmoril.esm", "Skyrim.esm", sources) == PluginCellRelation::Absent);
	REQUIRE(ClassifyPluginCell("", "Skyrim.esm", sources) == PluginCellRelation::Absent);
}
TEST_CASE("DX12 Resources filters a selected plugin's interior catalog", "[dx12_resources]")
{
	using namespace DX12ResourceService;
	PluginInteriorEntry a{ { "Unslaad.esm", 0x4FD2B }, "UnslaadGuestRoom", "Guest Room",
		PluginCellRelation::Defined };
	PluginInteriorEntry b{ { "Skyrim.esm", 0x1234 }, "WhiterunBreezehome", "Breezehome",
		PluginCellRelation::Override };
	REQUIRE(MatchesInteriorSearch(a, "guest"));
	REQUIRE(MatchesInteriorSearch(a, "4FD2B"));
	REQUIRE(MatchesInteriorSearch(a, "unslaad.esm"));
	REQUIRE_FALSE(MatchesInteriorSearch(a, "Glenmoril"));
	REQUIRE(MatchesInteriorSearch(b, "BREEZE"));
	std::vector<PluginInteriorEntry> entries{ a, b };
	SortInteriorCatalog(entries);
	REQUIRE(entries.front().displayName == "Breezehome");
	// The overridden CELL always resolves through its original source plugin.
	REQUIRE(entries.front().identity.plugin == "Skyrim.esm");
}

TEST_CASE("DX12 Resources known interiors bypass automatic save/current-cell detection", "[dx12_resources]")
{
	using namespace DX12ResourceService;
	const std::optional<CellRequest> selected = CellRequest{ "Glenmoril.esm", 0x99F2 };
	// No latest-save hint and no currently loaded gameplay cell participate here.
	REQUIRE(CanPrepareKnownInterior(true, true, false, selected));
	REQUIRE_FALSE(CanPrepareKnownInterior(true, false, false, selected));
	REQUIRE_FALSE(CanPrepareKnownInterior(true, true, true, selected));
	REQUIRE_FALSE(CanPrepareKnownInterior(false, true, false, selected));
	REQUIRE_FALSE(CanPrepareKnownInterior(true, true, false, {}));
	REQUIRE_FALSE(CanPrepareKnownInterior(true, true, false,
		CellRequest{ "Glenmoril.esm", 0 }));
}
