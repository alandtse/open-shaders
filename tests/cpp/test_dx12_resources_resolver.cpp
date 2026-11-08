#include <catch2/catch_test_macros.hpp>
#include "Features/DX12Resources/AutoCellResolver.h"

TEST_CASE("Automatic DX12 CELL detection resolves unique save EditorID first", "[dx12_resources]")
{
    using namespace DX12ResourceService;
    const std::vector<InteriorCellCandidate> cells{
        {{"Skyrim.esm", 0x101}, "BluePalace01", "Blue Palace"},
        {{"MyMod.esp", 0x202}, "MyModRoom", "Blue Palace"}
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
