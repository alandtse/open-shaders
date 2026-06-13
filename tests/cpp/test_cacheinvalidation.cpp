// Unit tests for Util::CacheInvalidation (disk shader-cache mismatch
// classification + feature-aware partial invalidation). The conservative
// fallbacks tested here are exactly the paths a live game session almost
// never exercises.

#include "Utils/CacheInvalidation.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <process.h>

using namespace Util::CacheInvalidation;
namespace fs = std::filesystem;

namespace
{
	struct TempDir
	{
		fs::path path;
		TempDir()
		{
			static std::atomic<unsigned> counter{ 0 };
			path = fs::temp_directory_path() / std::format("csci_{}_{}_{}", ::_getpid(),
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
		std::ofstream(p) << text;
	}

	FeatureState Feat(std::string shortName, bool loaded, std::string version = "1-0-0", std::string define = "DEF")
	{
		return { shortName, shortName + " Name", loaded, version, define };
	}
}

TEST_CASE("ClassifyMismatches: clean cache yields no mismatches", "[cacheinvalidation]")
{
	auto m = ClassifyMismatches("1-7-1-0", "1-7-1-0",
		{ Feat("A", true), Feat("B", false) },
		{ { "A", { true, "1-0-0" } }, { "B", { false, std::nullopt } } });
	REQUIRE(m.empty());
}

TEST_CASE("ClassifyMismatches: plugin version", "[cacheinvalidation]")
{
	SECTION("changed")
	{
		auto m = ClassifyMismatches("1-7-2-0", "1-7-1-0", {}, {});
		REQUIRE(m.size() == 1);
		CHECK(m[0].kind == CacheMismatch::Kind::PluginVersion);
	}
	SECTION("missing Info.ini")
	{
		auto m = ClassifyMismatches("1-7-1-0", std::nullopt, {}, {});
		REQUIRE(m.size() == 1);
		CHECK(m[0].kind == CacheMismatch::Kind::PluginVersion);
	}
}

TEST_CASE("ClassifyMismatches: enabled flips in both directions", "[cacheinvalidation]")
{
	SECTION("feature uninstalled/boot-disabled vs cache built with it")
	{
		auto m = ClassifyMismatches("1", "1", { Feat("UW", false) }, { { "UW", { true, "1-0-0" } } });
		REQUIRE(m.size() == 1);
		CHECK(m[0].kind == CacheMismatch::Kind::EnabledFlip);
		CHECK(m[0].detail.find("uninstalled or disabled") != std::string::npos);
		CHECK(m[0].nowPresent == false);  // removed direction → UI shows the "_removed" string
	}
	SECTION("feature installed vs cache built without it")
	{
		auto m = ClassifyMismatches("1", "1", { Feat("UW", true) }, { { "UW", { false, std::nullopt } } });
		REQUIRE(m.size() == 1);
		CHECK(m[0].kind == CacheMismatch::Kind::EnabledFlip);
		CHECK(m[0].detail.find("built without it") != std::string::npos);
		CHECK(m[0].nowPresent == true);  // added direction → UI shows the "_added" string
	}
	SECTION("feature absent from manifest entirely counts as flip when loaded")
	{
		auto m = ClassifyMismatches("1", "1", { Feat("New", true) }, {});
		REQUIRE(m.size() == 1);
		CHECK(m[0].kind == CacheMismatch::Kind::EnabledFlip);
	}
}

TEST_CASE("ClassifyMismatches: feature version change and null-version guard", "[cacheinvalidation]")
{
	SECTION("version changed")
	{
		auto m = ClassifyMismatches("1", "1", { Feat("A", true, "2-0-0") }, { { "A", { true, "1-0-0" } } });
		REQUIRE(m.size() == 1);
		CHECK(m[0].kind == CacheMismatch::Kind::FeatureVersion);
	}
	SECTION("enabled section with no Version value does not crash (old code strcmp'd null)")
	{
		auto m = ClassifyMismatches("1", "1", { Feat("A", true) }, { { "A", { true, std::nullopt } } });
		REQUIRE(m.size() == 1);
		CHECK(m[0].kind == CacheMismatch::Kind::FeatureVersion);
		CHECK(m[0].detail.find("<none>") != std::string::npos);
	}
	SECTION("unloaded feature's stale version is ignored")
	{
		auto m = ClassifyMismatches("1", "1", { Feat("A", false) }, { { "A", { false, "0-0-1" } } });
		REQUIRE(m.empty());
	}
}

TEST_CASE("RootShaderReferencesToken: closure search", "[cacheinvalidation]")
{
	TempDir t;
	auto shaders = t.path / "Shaders";
	Write(shaders / "Root.hlsl", "#include \"Common/A.hlsli\"\nfloat4 main() { return 0; }\n");
	Write(shaders / "Common/A.hlsli", "#include \"Common/B.hlsli\"\n");
	Write(shaders / "Common/B.hlsli", "#if defined(MY_TOKEN)\nfloat x;\n#endif\n");

	SECTION("token found through nested includes")
	{
		auto r = RootShaderReferencesToken(shaders / "Root.hlsl", "MY_TOKEN", shaders);
		REQUIRE(r.has_value());
		CHECK(*r == true);
	}
	SECTION("absent token reports false")
	{
		auto r = RootShaderReferencesToken(shaders / "Root.hlsl", "OTHER_TOKEN", shaders);
		REQUIRE(r.has_value());
		CHECK(*r == false);
	}
	SECTION("missing root file reports nullopt (conservative)")
	{
		auto r = RootShaderReferencesToken(shaders / "Nope.hlsl", "MY_TOKEN", shaders);
		CHECK_FALSE(r.has_value());
	}
	SECTION("include cycles terminate")
	{
		Write(shaders / "Cyc1.hlsl", "#include \"Cyc2.hlsli\"\n");
		Write(shaders / "Cyc2.hlsli", "#include \"Cyc1.hlsl\"\nMY_TOKEN\n");
		auto r = RootShaderReferencesToken(shaders / "Cyc1.hlsl", "MY_TOKEN", shaders);
		REQUIRE(r.has_value());
		CHECK(*r == true);
	}
	SECTION("includer-relative include resolution")
	{
		Write(shaders / "Sub/Local.hlsl", "#include \"LocalDep.hlsli\"\n");
		Write(shaders / "Sub/LocalDep.hlsli", "MY_TOKEN\n");
		auto r = RootShaderReferencesToken(shaders / "Sub/Local.hlsl", "MY_TOKEN", shaders);
		REQUIRE(r.has_value());
		CHECK(*r == true);
	}
	SECTION("identifier-boundary: superstring token does not match")
	{
		Write(shaders / "Super.hlsl", "#if defined(MY_TOKENX)\n#endif\n");
		auto r = RootShaderReferencesToken(shaders / "Super.hlsl", "MY_TOKEN", shaders);
		REQUIRE(r.has_value());
		CHECK(*r == false);
	}
	SECTION("relative-spelling cycle terminates (visited is normalized)")
	{
		Write(shaders / "Rel1.hlsl", "#include \"Sub/../Rel2.hlsli\"\n");
		Write(shaders / "Rel2.hlsli", "#include \"Sub/../Rel1.hlsl\"\n");
		Write(shaders / "Sub/keep.txt", "");
		auto r = RootShaderReferencesToken(shaders / "Rel1.hlsl", "MY_TOKEN", shaders);
		REQUIRE(r.has_value());
		CHECK(*r == false);
	}
	SECTION("unresolvable include is skipped, not fatal")
	{
		Write(shaders / "Gated.hlsl", "#include \"NotInstalledFeature/X.hlsli\"\n");
		auto r = RootShaderReferencesToken(shaders / "Gated.hlsl", "MY_TOKEN", shaders);
		REQUIRE(r.has_value());
		CHECK(*r == false);
	}
}

TEST_CASE("TryPartialInvalidation: deletes exactly the referencing dirs", "[cacheinvalidation]")
{
	TempDir t;
	auto shaders = t.path / "Shaders";
	auto cache = t.path / "ShaderCache";
	Write(shaders / "Water.hlsl", "#if defined(UNIFIED_WATER)\n#endif\n");
	Write(shaders / "Sky.hlsl", "float4 main() { return 0; }\n");
	Write(cache / "Water/1.pso", "blob");
	Write(cache / "Sky/1.pso", "blob");

	size_t deleted = 0, kept = 0;
	REQUIRE(TryPartialInvalidation(cache, shaders, { "UNIFIED_WATER" }, &deleted, &kept));
	CHECK(deleted == 1);
	CHECK(kept == 1);
	CHECK_FALSE(fs::exists(cache / "Water"));
	CHECK(fs::exists(cache / "Sky/1.pso"));
}

TEST_CASE("TryPartialInvalidation: conservative fallbacks", "[cacheinvalidation]")
{
	TempDir t;
	auto shaders = t.path / "Shaders";
	auto cache = t.path / "ShaderCache";
	Write(shaders / "Water.hlsl", "x\n");
	Write(cache / "Water/1.pso", "blob");

	SECTION("empty define forces full wipe")
	{
		CHECK_FALSE(TryPartialInvalidation(cache, shaders, { "" }));
		CHECK(fs::exists(cache / "Water/1.pso"));  // nothing touched on refusal
	}
	SECTION("cache dir without matching root source forces full wipe")
	{
		Write(cache / "Orphan/1.pso", "blob");
		CHECK_FALSE(TryPartialInvalidation(cache, shaders, { "DEF" }));
	}
	SECTION("no defines deletes nothing and succeeds")
	{
		size_t deleted = 9, kept = 0;
		REQUIRE(TryPartialInvalidation(cache, shaders, {}, &deleted, &kept));
		CHECK(deleted == 0);
		CHECK(kept == 1);
	}
}
