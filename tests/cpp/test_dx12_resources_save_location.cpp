#include "Features/DX12Resources/SaveLocation.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>

TEST_CASE("DX12 Resources safely parses location from an SE save header", "[dx12_resources]")
{
	std::vector<uint8_t> bytes;
	const auto add32 = [&](uint32_t n) {
		for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(n >> (8 * i)));
	};
	const auto addString = [&](std::string_view str) {
		bytes.push_back(uint8_t(str.size()));
		bytes.push_back(uint8_t(str.size() >> 8));
		bytes.insert(bytes.end(), str.begin(), str.end());
	};
	for (char ch : std::string_view("TESV_SAVEGAME")) bytes.push_back(uint8_t(ch));
	add32(64);  // header bytes (bounded)
	add32(12);  // SE format version
	add32(8);   // save number
	addString("Hero");
	add32(40);
	addString("Blue Palace");
	REQUIRE(DX12ResourceService::ReadEssLocation(bytes) == "Blue Palace");
	bytes[0] = 'X';
	REQUIRE_FALSE(DX12ResourceService::ReadEssLocation(bytes));
}
TEST_CASE("DX12 Resources save filename hint never becomes a form identity", "[dx12_resources]")
{
	REQUIRE(DX12ResourceService::SaveFileCellEditorId(
				"Save141_38DBE035_0_41726173_Mzulft01_011006_20250928182132_18_1") == "Mzulft01");
	REQUIRE(DX12ResourceService::SameCellHint("Mzulft01", "mZULFT01"));
	REQUIRE(DX12ResourceService::SaveFileCellEditorId("Autosave") == "");
}
