// Unit tests for FurShellAssets::AnyShellTextures (loose and BSA shell texture detection).

#include "Features/FurShells/AssetScan.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	struct TempDir
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / "cs_furshell_scan_test";

		TempDir()
		{
			std::filesystem::remove_all(path);
			std::filesystem::create_directories(path);
		}
		~TempDir() { std::filesystem::remove_all(path); }
	};

	void WriteArchive(const std::filesystem::path& a_path, const std::vector<std::string>& a_names, uint32_t a_flags)
	{
		const std::string folder = "textures\armor";
		std::string names;
		for (const auto& name : a_names)
			names += name + '\0';

		const std::array<uint32_t, 8> header{ 105, 36, a_flags, 1, static_cast<uint32_t>(a_names.size()), static_cast<uint32_t>(folder.size() + 1), static_cast<uint32_t>(names.size()), 0x80 };
		std::ofstream out(a_path, std::ios::binary);
		out.write("BSA\0", 4);
		out.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
		const uint64_t hash = 0;
		const uint32_t count = static_cast<uint32_t>(a_names.size());
		const uint32_t zero = 0;
		out.write(reinterpret_cast<const char*>(&hash), 8);
		out.write(reinterpret_cast<const char*>(&count), 4);
		out.write(reinterpret_cast<const char*>(&zero), 4);
		out.write(reinterpret_cast<const char*>(&hash), 8);
		const char nameLength = static_cast<char>(folder.size() + 1);
		out.write(&nameLength, 1);
		out.write(folder.c_str(), static_cast<std::streamsize>(folder.size() + 1));
		for (size_t i = 0; i < a_names.size(); ++i) {
			out.write(reinterpret_cast<const char*>(&hash), 8);
			out.write(reinterpret_cast<const char*>(&zero), 4);
			out.write(reinterpret_cast<const char*>(&zero), 4);
		}
		out.write(names.data(), static_cast<std::streamsize>(names.size()));
	}
}

TEST_CASE("Asset scan reports no shells for an empty Data folder", "[furshells]")
{
	TempDir dir;
	REQUIRE_FALSE(FurShellAssets::AnyShellTextures(dir.path));
}

TEST_CASE("Asset scan finds a loose shell regardless of case", "[furshells]")
{
	TempDir dir;
	std::filesystem::create_directories(dir.path / "textures" / "armor");
	std::ofstream(dir.path / "textures" / "armor" / "Plate_Shell.DDS").put('x');
	REQUIRE(FurShellAssets::AnyShellTextures(dir.path));
}

TEST_CASE("Asset scan reads the BSA file name table", "[furshells]")
{
	TempDir dir;
	WriteArchive(dir.path / "plain.bsa", { "plate.dds", "plate_n.dds" }, 0x3);
	REQUIRE_FALSE(FurShellAssets::AnyShellTextures(dir.path));

	WriteArchive(dir.path / "fur.bsa", { "plate.dds", "plate_shell.dds" }, 0x3);
	REQUIRE(FurShellAssets::AnyShellTextures(dir.path));
}

TEST_CASE("Asset scan assumes shells for an archive without file names", "[furshells]")
{
	TempDir dir;
	WriteArchive(dir.path / "anonymous.bsa", { "plate.dds" }, 0x1);
	REQUIRE(FurShellAssets::AnyShellTextures(dir.path));
}

TEST_CASE("Asset scan handles non-ANSI file names", "[furshells]")
{
	TempDir dir;
	const auto folder = dir.path / "textures" / std::filesystem::path(L"毛皮");
	std::filesystem::create_directories(folder);
	std::ofstream(folder / std::filesystem::path(L"盔甲.dds")).put('x');
	REQUIRE_FALSE(FurShellAssets::AnyShellTextures(dir.path));

	std::ofstream(folder / std::filesystem::path(L"盔甲_shell.dds")).put('x');
	REQUIRE(FurShellAssets::AnyShellTextures(dir.path));
}

TEST_CASE("Asset scan assumes shells for a corrupt archive header", "[furshells]")
{
	TempDir dir;
	WriteArchive(dir.path / "corrupt.bsa", { "plate.dds" }, 0x3);
	{
		std::fstream file(dir.path / "corrupt.bsa", std::ios::binary | std::ios::in | std::ios::out);
		file.seekp(4 + 6 * sizeof(uint32_t));
		const uint32_t hugeNameBlock = 0xFFFFFFF0u;
		file.write(reinterpret_cast<const char*>(&hugeNameBlock), sizeof(hugeNameBlock));
	}
	REQUIRE(FurShellAssets::AnyShellTextures(dir.path));
}
