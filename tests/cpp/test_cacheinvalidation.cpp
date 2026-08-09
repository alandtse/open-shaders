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

TEST_CASE("TryPartialInvalidation: delete failure on the only candidate is not destructive", "[cacheinvalidation]")
{
	TempDir t;
	auto shaders = t.path / "Shaders";
	auto cache = t.path / "ShaderCache";
	Write(shaders / "Water.hlsl", "#if defined(UNIFIED_WATER)\n#endif\n");
	Write(cache / "Water/1.pso", "blob");

	// An open (not delete-sharing) file handle blocks Windows from removing
	// its directory, forcing remove_all() to throw. Nothing else was queued
	// for deletion, so the cache is left exactly as it started -- not the
	// "already partially gone" case the destructive flag exists to signal.
	std::ofstream held(cache / "Water/1.pso", std::ios::binary | std::ios::app);
	REQUIRE(held.is_open());

	bool destructive = false;
	CHECK_FALSE(TryPartialInvalidation(cache, shaders, { "UNIFIED_WATER" }, nullptr, nullptr, &destructive));
	CHECK_FALSE(destructive);

	held.close();
}

TEST_CASE("TryPartialInvalidation: failure after a prior deletion is destructive", "[cacheinvalidation]")
{
	TempDir t;
	auto shaders = t.path / "Shaders";
	auto cache = t.path / "ShaderCache";
	Write(shaders / "Fog.hlsl", "#if defined(UNIFIED_WATER)\n#endif\n");
	Write(shaders / "Water.hlsl", "#if defined(UNIFIED_WATER)\n#endif\n");
	Write(cache / "Fog/1.pso", "blob");
	Write(cache / "Water/1.pso", "blob");

	// NTFS enumerates a directory's entries via its $I30 B-tree index, sorted
	// by filename -- "Fog" iterates before "Water", so this deterministically
	// exercises delete-succeeds-then-delete-fails, not delete-fails-first.
	std::ofstream held(cache / "Water/1.pso", std::ios::binary | std::ios::app);
	REQUIRE(held.is_open());

	bool destructive = false;
	CHECK_FALSE(TryPartialInvalidation(cache, shaders, { "UNIFIED_WATER" }, nullptr, nullptr, &destructive));
	CHECK(destructive);
	CHECK_FALSE(fs::exists(cache / "Fog"));  // confirms Fog really was deleted first

	held.close();
}

TEST_CASE("TryPartialInvalidation: partial delete within one directory is destructive", "[cacheinvalidation]")
{
	// remove_all() recurses into a directory's own contents; a single toDelete
	// entry with one locked file among several must still report destructive,
	// since its unlocked siblings get removed before the locked one throws.
	TempDir t;
	auto shaders = t.path / "Shaders";
	auto cache = t.path / "ShaderCache";
	Write(shaders / "Water.hlsl", "#if defined(UNIFIED_WATER)\n#endif\n");
	Write(cache / "Water/1.pso", "blob");
	Write(cache / "Water/2.pso", "blob");

	std::ofstream held(cache / "Water/2.pso", std::ios::binary | std::ios::app);
	REQUIRE(held.is_open());

	bool destructive = false;
	CHECK_FALSE(TryPartialInvalidation(cache, shaders, { "UNIFIED_WATER" }, nullptr, nullptr, &destructive));
	CHECK(destructive);
	CHECK_FALSE(fs::exists(cache / "Water/1.pso"));  // the unlocked sibling is really gone
	CHECK(fs::exists(cache / "Water/2.pso"));        // the locked file survives

	held.close();
}

TEST_CASE("BackupCacheDirectory and RestoreCacheDirectory: transactional swap logic", "[cacheinvalidation]")
{
	TempDir t;
	auto active = t.path / "ShaderCache";
	auto previous = t.path / "ShaderCache.Previous";
	auto swap = t.path / "ShaderCache.Swap";

	Write(active / "Info.ini", "[Cache]\nPluginVersion=1-7-1-0\n");
	Write(active / "blob1.pso", "blob1");

	SECTION("Backup when no previous exists")
	{
		std::string error;
		REQUIRE(BackupCacheDirectory(active, previous, swap, &error));
		CHECK(fs::exists(previous / "Info.ini"));
		CHECK(fs::exists(previous / "blob1.pso"));
		CHECK(fs::exists(active));
		CHECK_FALSE(fs::exists(active / "Info.ini"));
		CHECK_FALSE(fs::exists(swap));
	}

	SECTION("Backup when previous exists")
	{
		Write(previous / "Info.ini", "[Cache]\nPluginVersion=1-7-0-0\n");
		Write(previous / "blob0.pso", "blob0");

		std::string error;
		REQUIRE(BackupCacheDirectory(active, previous, swap, &error));
		CHECK(fs::exists(previous / "Info.ini"));
		CHECK(fs::exists(previous / "blob1.pso"));
		CHECK_FALSE(fs::exists(previous / "blob0.pso"));
		CHECK(fs::exists(active));
		CHECK_FALSE(fs::exists(swap));
	}

	SECTION("Restore previous cache")
	{
		Write(previous / "Info.ini", "[Cache]\nPluginVersion=1-7-0-0\n");
		Write(previous / "blob0.pso", "blob0");

		std::string error, warning;
		REQUIRE(RestoreCacheDirectory(active, previous, swap, &error, &warning));
		CHECK(fs::exists(active / "Info.ini"));
		CHECK(fs::exists(active / "blob0.pso"));
		CHECK_FALSE(fs::exists(active / "blob1.pso"));
		CHECK(fs::exists(previous / "Info.ini"));
		CHECK(fs::exists(previous / "blob1.pso"));
		CHECK_FALSE(fs::exists(swap));
	}
}

TEST_CASE("BackupCacheDirectory: transactional failure paths leave state untouched", "[cacheinvalidation]")
{
	TempDir t;
	auto active = t.path / "ShaderCache";
	auto previous = t.path / "ShaderCache.Previous";
	auto swap = t.path / "ShaderCache.Swap";

	Write(active / "Info.ini", "[Cache]\nPluginVersion=1-7-1-0\n");
	Write(active / "blob1.pso", "blob1");

	SECTION("fails when the rollback destination's parent does not exist")
	{
		// No previous cache yet, so this hits the active->previous rename directly
		// (the "could not move the active cache into the rollback slot" branch).
		auto missingParentPrevious = t.path / "no-such-dir" / "Previous";

		std::string error;
		CHECK_FALSE(BackupCacheDirectory(active, missingParentPrevious, swap, &error));
		CHECK(error.find("rollback slot") != std::string::npos);
		// Nothing moved: active is exactly as it was, no rollback slot was created.
		CHECK(fs::exists(active / "Info.ini"));
		CHECK(fs::exists(active / "blob1.pso"));
		CHECK_FALSE(fs::exists(missingParentPrevious));
	}

	SECTION("fails when stashing an existing rollback cache fails, previous is untouched")
	{
		Write(previous / "Info.ini", "[Cache]\nPluginVersion=1-7-0-0\n");
		Write(previous / "blob0.pso", "blob0");
		auto missingParentSwap = t.path / "no-such-dir" / "Swap";

		std::string error;
		CHECK_FALSE(BackupCacheDirectory(active, previous, missingParentSwap, &error));
		CHECK(error.find("old rollback cache") != std::string::npos);
		// The stash-previous-into-swap step failed before touching active at all.
		CHECK(fs::exists(active / "Info.ini"));
		CHECK(fs::exists(active / "blob1.pso"));
		CHECK(fs::exists(previous / "Info.ini"));
		CHECK(fs::exists(previous / "blob0.pso"));
	}
}

TEST_CASE("RestoreCacheDirectory: transactional failure path leaves the rollback cache intact", "[cacheinvalidation]")
{
	TempDir t;
	auto previous = t.path / "ShaderCache.Previous";
	auto swap = t.path / "ShaderCache.Swap";
	Write(previous / "Info.ini", "[Cache]\nPluginVersion=1-7-0-0\n");
	Write(previous / "blob0.pso", "blob0");

	SECTION("fails when the active slot's parent does not exist")
	{
		// Active doesn't exist, so this skips the stash-active-to-swap step and
		// hits the previous->active rename directly ("could not move the
		// rollback cache into the active slot").
		auto missingParentActive = t.path / "no-such-dir" / "ShaderCache";

		std::string error, warning;
		CHECK_FALSE(RestoreCacheDirectory(missingParentActive, previous, swap, &error, &warning));
		CHECK(error.find("active slot") != std::string::npos);
		CHECK(warning.empty());
		// Nothing moved: the rollback cache is exactly as it was.
		CHECK(fs::exists(previous / "Info.ini"));
		CHECK(fs::exists(previous / "blob0.pso"));
		CHECK_FALSE(fs::exists(missingParentActive));
	}
}

TEST_CASE("OnlyEnabledFlips / HasFailedFeature / FindMatchBlockingFeature: decision predicates", "[cacheinvalidation]")
{
	const auto flip = [](std::string shortName, bool nowPresent, bool nowFailed = false) {
		return CacheMismatch{ CacheMismatch::Kind::EnabledFlip, shortName, shortName + " Name", "detail", nowPresent, nowFailed };
	};
	const auto versionBump = [](std::string shortName) {
		return CacheMismatch{ CacheMismatch::Kind::FeatureVersion, shortName, shortName + " Name", "detail", true };
	};

	SECTION("OnlyEnabledFlips: true for pure flips, false once a version bump is mixed in")
	{
		CHECK(OnlyEnabledFlips({ flip("A", true), flip("B", false) }));
		CHECK_FALSE(OnlyEnabledFlips({ flip("A", true), versionBump("B") }));
		CHECK(OnlyEnabledFlips({}));  // vacuously true, matches std::ranges::all_of on empty
	}

	SECTION("HasFailedFeature: only 'cache had it, now off, and genuinely failed to load' counts")
	{
		// Cache had A, A is gone now, and it genuinely failed to load: broken install.
		CHECK(HasFailedFeature({ flip("A", false, true) }));
		// Same mismatch, but not a load failure (deliberate/default disable): not missing.
		CHECK_FALSE(HasFailedFeature({ flip("A", false, false) }));
		// "Added" direction (nowPresent=true) never counts, regardless of nowFailed.
		CHECK_FALSE(HasFailedFeature({ flip("A", true, true) }));
		// A FeatureVersion mismatch is never treated as a missing-feature case.
		CHECK_FALSE(HasFailedFeature({ versionBump("A") }));
	}

	SECTION("FindMatchBlockingFeature: only a removed-and-failed-to-load feature blocks Match")
	{
		const std::vector<CacheMismatch> none;
		CHECK(FindMatchBlockingFeature(none) == nullptr);

		// Added direction never blocks, even if that feature also happens to have failed to load.
		const std::vector<CacheMismatch> addedOnly{ flip("A", true, true) };
		CHECK(FindMatchBlockingFeature(addedOnly) == nullptr);

		// Removed direction, but the feature just isn't installed (no failedLoadedMessage): no block.
		const std::vector<CacheMismatch> removedNotFailed{ flip("A", false, false) };
		CHECK(FindMatchBlockingFeature(removedNotFailed) == nullptr);

		// Removed direction and the feature genuinely failed to load: blocks, returns that mismatch.
		// The mismatch vector must outlive the returned pointer, so it's a named local, not a temporary.
		const std::vector<CacheMismatch> removedAndFailed{ flip("A", false, true) };
		const auto* blocking = FindMatchBlockingFeature(removedAndFailed);
		REQUIRE(blocking != nullptr);
		CHECK(blocking->shortName == "A");

		// First matching candidate wins when more than one qualifies.
		const std::vector<CacheMismatch> twoCandidates{ flip("A", false, true), flip("B", false, true) };
		const auto* firstOfTwo = FindMatchBlockingFeature(twoCandidates);
		REQUIRE(firstOfTwo != nullptr);
		CHECK(firstOfTwo->shortName == "A");
	}
}

TEST_CASE("AreCacheMismatchesRestorable / TrySetRestoreCandidate: rollback restore eligibility", "[cacheinvalidation]")
{
	const auto flip = [](std::string shortName, bool nowPresent, bool nowFailed = false) {
		return CacheMismatch{ CacheMismatch::Kind::EnabledFlip, shortName, shortName + " Name", "detail", nowPresent, nowFailed };
	};
	const auto versionBump = [](std::string shortName) {
		return CacheMismatch{ CacheMismatch::Kind::FeatureVersion, shortName, shortName + " Name", "detail", true };
	};

	SECTION("AreCacheMismatchesRestorable: empty, mixed-kind, and broken-install all disqualify")
	{
		CHECK_FALSE(AreCacheMismatchesRestorable({}));

		const std::vector<CacheMismatch> pureFlipsNotFailed{ flip("A", false, false) };
		CHECK(AreCacheMismatchesRestorable(pureFlipsNotFailed));

		// Cache had it, now missing, and it genuinely failed to load: broken install.
		const std::vector<CacheMismatch> pureFlipsFailed{ flip("A", false, true) };
		CHECK_FALSE(AreCacheMismatchesRestorable(pureFlipsFailed));

		const std::vector<CacheMismatch> mixedKinds{ flip("A", false, false), versionBump("B") };
		CHECK_FALSE(AreCacheMismatchesRestorable(mixedKinds));
	}

	SECTION("TrySetRestoreCandidate: on-disk check gates it even when mismatches are otherwise restorable")
	{
		const std::vector<CacheMismatch> restorable{ flip("A", false, false) };
		bool available = false;
		std::vector<CacheMismatch> recorded;

		CHECK_FALSE(TrySetRestoreCandidate(restorable, /*previousCacheOnDisk=*/false, available, recorded));
		CHECK_FALSE(available);
		CHECK(recorded.empty());
	}

	SECTION("TrySetRestoreCandidate: succeeds and records the mismatches when both conditions hold")
	{
		const std::vector<CacheMismatch> restorable{ flip("A", false, false), flip("B", true) };
		bool available = false;
		std::vector<CacheMismatch> recorded;

		CHECK(TrySetRestoreCandidate(restorable, /*previousCacheOnDisk=*/true, available, recorded));
		CHECK(available);
		REQUIRE(recorded.size() == 2);
		CHECK(recorded[0].shortName == "A");
		CHECK(recorded[1].shortName == "B");
	}

	SECTION("TrySetRestoreCandidate: not-restorable mismatches fail even with the cache on disk")
	{
		const std::vector<CacheMismatch> brokenInstall{ flip("A", false, true) };
		bool available = false;
		std::vector<CacheMismatch> recorded;

		CHECK_FALSE(TrySetRestoreCandidate(brokenInstall, /*previousCacheOnDisk=*/true, available, recorded));
		CHECK_FALSE(available);
		CHECK(recorded.empty());
	}

	SECTION("TrySetRestoreCandidate: a failed call does not clobber a previously-set candidate")
	{
		// Mirrors the real call sites (CommitFeatureSetChange, RestorePreviousDiskCache),
		// which re-call this after RefreshPreviousDiskCacheInfo already reset outAvailable
		// to false -- a failed re-check must leave that false, not silently flip it true
		// from stale in/out aliasing.
		bool available = true;
		std::vector<CacheMismatch> recorded{ flip("Stale", false) };

		const std::vector<CacheMismatch> notRestorable{ versionBump("A") };
		CHECK_FALSE(TrySetRestoreCandidate(notRestorable, /*previousCacheOnDisk=*/true, available, recorded));
		// Untouched on failure: the function returns before writing either out-param.
		CHECK(available);
		REQUIRE(recorded.size() == 1);
		CHECK(recorded[0].shortName == "Stale");
	}
}
