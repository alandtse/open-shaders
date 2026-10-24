// Unit tests for Util::ContentHash (XXH3-128 content hashing for the shader
// disk cache). Correctness properties that matter here: determinism, actual
// content-sensitivity, and CRLF/LF normalization -- not xxHash's own
// collision behavior, which is out of scope for this repo to re-verify.

#include "Utils/ContentHash.h"
#include "Utils/StandaloneShaderCacheKey.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <process.h>

using namespace Util::ContentHash;
namespace fs = std::filesystem;

namespace
{
	struct TempDir
	{
		fs::path path;
		TempDir()
		{
			static std::atomic<unsigned> counter{ 0 };
			path = fs::temp_directory_path() / std::format("csch_{}_{}_{}", ::_getpid(),
												   std::chrono::high_resolution_clock::now().time_since_epoch().count(), counter++);
			fs::create_directories(path);
		}
		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	};

	void Write(const fs::path& p, const std::string& text)
	{
		fs::create_directories(p.parent_path());
		std::ofstream ofs(p, std::ios::binary);
		ofs << text;
	}
}

TEST_CASE("HashString is deterministic and content-sensitive", "[ContentHash]")
{
	CHECK(HashString("abc") == HashString("abc"));
	CHECK_FALSE(HashString("abc") == HashString("abd"));
	CHECK_FALSE(HashString("") == HashString("a"));
}

TEST_CASE("CombineHashes is order-sensitive", "[ContentHash]")
{
	const auto a = HashString("a");
	const auto b = HashString("b");
	CHECK(CombineHashes(a, b) == CombineHashes(a, b));
	CHECK_FALSE(CombineHashes(a, b) == CombineHashes(b, a));
}

TEST_CASE("HashFile matches HashString on the file's bytes", "[ContentHash]")
{
	TempDir dir;
	const auto file = dir.path / "shader.hlsl";
	Write(file, "float4 main() { return 0; }");

	const auto fileHash = HashFile(file);
	REQUIRE(fileHash.has_value());
	CHECK(*fileHash == HashString("float4 main() { return 0; }"));
}

TEST_CASE("HashFile normalizes CRLF to LF", "[ContentHash]")
{
	TempDir dir;
	const auto crlfFile = dir.path / "crlf.hlsli";
	const auto lfFile = dir.path / "lf.hlsli";
	Write(crlfFile, "line1\r\nline2\r\n");
	Write(lfFile, "line1\nline2\n");

	const auto crlfHash = HashFile(crlfFile);
	const auto lfHash = HashFile(lfFile);
	REQUIRE(crlfHash.has_value());
	REQUIRE(lfHash.has_value());
	CHECK(*crlfHash == *lfHash);
}

TEST_CASE("HashFile detects real content changes across line-ending styles", "[ContentHash]")
{
	TempDir dir;
	const auto file = dir.path / "changed.hlsli";
	Write(file, "value = 1\r\n");
	const auto before = HashFile(file);
	Write(file, "value = 2\r\n");
	const auto after = HashFile(file);
	REQUIRE(before.has_value());
	REQUIRE(after.has_value());
	CHECK_FALSE(*before == *after);
}

TEST_CASE("HashFile returns nullopt for a missing file", "[ContentHash]")
{
	TempDir dir;
	CHECK_FALSE(HashFile(dir.path / "does-not-exist.hlsl").has_value());
}

TEST_CASE("Standalone shader keys survive install paths and macro ordering", "[ContentHash][ShaderCache]")
{
	using namespace Util::StandaloneShaderCacheKey;
	const auto first = SourceName(L"Data\\Shaders\\Effects11\\QuadVS.hlsl");
	CHECK(first == "Effects11/QuadVS.hlsl");
	CHECK(SourceName(L"D:\\Games\\Skyrim\\Data\\Shaders\\Effects11\\QuadVS.hlsl") == first);
	CHECK(SourceName(L"Data/Shaders/Utility.hlsl") == "Utility.hlsl");
	CHECK(SourcePath(L"Data/Shaders/Effects11/QuadVS.hlsl") == fs::path(L"Effects11/QuadVS.hlsl"));
	CHECK(SourcePath(L"D:/Games/Skyrim/Data/Shaders/Effects11/../Utility.hlsl") == fs::path(L"Utility.hlsl"));
	CHECK(SourcePath(L"Effects11/QuadVS.hlsl") == fs::path(L"Effects11/QuadVS.hlsl"));
	CHECK(SourcePath(L"Data/Shaders/Utility.hlsl").parent_path().empty());
	CHECK(SourceName(L"Data/Shaders/Effects11/../Utility.hlsl") == "Utility.hlsl");
	const auto defines = CanonicalDefines({ { "VR", "" }, { "REVERSE_Z", "" }, { "VR", "" } });
	CHECK(defines == CanonicalDefines({ { "REVERSE_Z", "" }, { "VR", "" } }));
	const auto key = MakeKey(first, "main", "vs_5_0", 34818, defines);
	CHECK(key.ToHex() == "95db091fa85c9a6fb12d3f348b3fdd5e");
	CHECK_FALSE(key == MakeKey(first, "main", "ps_5_0", 34818, defines));
	CHECK_FALSE(key == MakeKey(first, "main", "vs_5_0", 34816, defines));
	CHECK_FALSE(key == MakeKey(first, "other", "vs_5_0", 34818, defines));
	CHECK_FALSE(key == MakeKey(first, "main", "vs_5_0", 34818, CanonicalDefines({ { "VR", "" } })));
	CHECK_FALSE(MakeKey(first, "main", "vs_5_0", 34818, { { "FLAG", std::nullopt } }) ==
				MakeKey(first, "main", "vs_5_0", 34818, { { "FLAG", "" } }));
}
