#include "Features/DX12Resources/CellRequest.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("DX12 Resources manual interior cell identity parsing", "[dx12_resources]")
{
	using DX12ResourceService::ParseCellRequest;
	const auto cell = ParseCellRequest("Skyrim.esm", "000123");
	REQUIRE(cell);
	REQUIRE(cell->localId == 0x123);
	REQUIRE(cell->plugin == "Skyrim.esm");
	REQUIRE(cell->Key() == "000123:Skyrim.esm");
	REQUIRE(ParseCellRequest("Interior Overhaul.ESP", "0xa1b"));
	REQUIRE_FALSE(ParseCellRequest("../Bad.esm", "123"));
	REQUIRE_FALSE(ParseCellRequest("Skyrim.esm", "0"));
	REQUIRE_FALSE(ParseCellRequest("Skyrim.esm", "nothex"));
	REQUIRE_FALSE(ParseCellRequest("Skyrim.esm", "1000000"));
	REQUIRE_FALSE(ParseCellRequest("Skyrim.txt", "123"));
}
