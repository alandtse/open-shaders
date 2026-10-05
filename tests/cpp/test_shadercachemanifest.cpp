// Unit tests for Util::ShaderCacheManifest (sidecar content-digest manifest
// for the shader disk cache). The property that matters most: a missing or
// corrupt manifest must read as "no entries", never as an error a caller
// could mistake for "trust this blob".

#include "Utils/ShaderCacheManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <process.h>

using namespace Util::ShaderCacheManifest;
namespace fs = std::filesystem;

namespace
{
	struct TempDir
	{
		fs::path path;
		TempDir()
		{
			static std::atomic<unsigned> counter{ 0 };
			path = fs::temp_directory_path() / std::format("cscm_{}_{}_{}", ::_getpid(),
												   std::chrono::high_resolution_clock::now().time_since_epoch().count(), counter++);
			fs::create_directories(path);
		}
		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	};
}

TEST_CASE("Load on a missing file leaves the manifest empty", "[ShaderCacheManifest]")
{
	TempDir dir;
	Manifest m;
	m.Load(dir.path / "Manifest.json");
	CHECK_FALSE(m.Get("Lighting/1A2B.pso").has_value());
}

TEST_CASE("Load on a corrupt file leaves the manifest empty, not an error", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto path = dir.path / "Manifest.json";
	std::ofstream(path) << "{ this is not valid json";
	Manifest m;
	m.Load(path);
	CHECK_FALSE(m.Get("Lighting/1A2B.pso").has_value());
}

TEST_CASE("Set then Save then Load round-trips an entry", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto path = dir.path / "Manifest.json";
	{
		Manifest m;
		m.Load(path);  // no file yet -- establishes the save path
		m.Set("Lighting/1A2B.pso", "deadbeefdeadbeefdeadbeefdeadbeef");
		REQUIRE(m.Save());
	}
	REQUIRE(fs::exists(path));
	Manifest m2;
	m2.Load(path);
	const auto v = m2.Get("Lighting/1A2B.pso");
	REQUIRE(v.has_value());
	CHECK(*v == "deadbeefdeadbeefdeadbeefdeadbeef");
}

TEST_CASE("Save with no changes since Load is a no-op that still reports success", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto path = dir.path / "Manifest.json";
	Manifest m;
	m.Load(path);
	CHECK(m.Save());
	CHECK_FALSE(fs::exists(path));  // nothing dirty -> nothing written
}

TEST_CASE("A later Set overwrites an earlier one for the same path", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto path = dir.path / "Manifest.json";
	Manifest m;
	m.Load(path);
	m.Set("Water/00.pso", "aaaa");
	m.Set("Water/00.pso", "bbbb");
	REQUIRE(m.Save());
	Manifest m2;
	m2.Load(path);
	CHECK(*m2.Get("Water/00.pso") == "bbbb");
}

TEST_CASE("PruneIf removes only entries the predicate flags, and reports the count", "[ShaderCacheManifest]")
{
	TempDir dir;
	Manifest m;
	m.Load(dir.path / "Manifest.json");
	m.Set("Water/00.pso", "aaaa");
	m.Set("Sky/01.pso", "bbbb");
	m.Set("Water/01.pso", "cccc");

	const size_t removed = m.PruneIf([](const std::string& relativePath) {
		return relativePath.starts_with("Water/");
	});

	CHECK(removed == 2);
	CHECK_FALSE(m.Get("Water/00.pso").has_value());
	CHECK_FALSE(m.Get("Water/01.pso").has_value());
	REQUIRE(m.Get("Sky/01.pso").has_value());
	CHECK(*m.Get("Sky/01.pso") == "bbbb");
}

TEST_CASE("PruneIf with nothing matching removes nothing and leaves entries intact", "[ShaderCacheManifest]")
{
	TempDir dir;
	Manifest m;
	m.Load(dir.path / "Manifest.json");
	m.Set("Sky/01.pso", "bbbb");

	const size_t removed = m.PruneIf([](const std::string&) { return false; });

	CHECK(removed == 0);
	REQUIRE(m.Get("Sky/01.pso").has_value());
}

TEST_CASE("Content keys round-trip alongside digests", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto file = dir.path / "Manifest.json";
	{
		Manifest m;
		m.Load(file);
		m.Set("Lighting/10.pso", "digest");
		m.SetContentKey("Lighting/10.pso", "key");
		REQUIRE(m.Save());
	}
	Manifest loaded;
	loaded.Load(file);
	CHECK(loaded.Get("Lighting/10.pso") == "digest");
	CHECK(loaded.GetContentKey("Lighting/10.pso") == "key");
	CHECK_FALSE(loaded.GetContentKey("Lighting/11.pso").has_value());
}

TEST_CASE("A manifest written before content keys existed loads with none", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto file = dir.path / "Manifest.json";
	std::ofstream(file) << R"({"schemaVersion":1,"entries":{"a.pso":"d"}})";
	Manifest m;
	m.Load(file);
	CHECK(m.Get("a.pso") == "d");
	CHECK_FALSE(m.GetContentKey("a.pso").has_value());
}

TEST_CASE("Malformed content keys are ignored without dropping digests", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto file = dir.path / "Manifest.json";
	std::ofstream(file) << R"({"entries":{"a.pso":"d"},"contentKeys":{"a.pso":7,"b.pso":"k"}})";
	Manifest m;
	m.Load(file);
	CHECK(m.Get("a.pso") == "d");
	CHECK_FALSE(m.GetContentKey("a.pso").has_value());
	CHECK(m.GetContentKey("b.pso") == "k");
}

TEST_CASE("SetContentKey alone is persisted by Save and Load of a missing file clears keys", "[ShaderCacheManifest]")
{
	TempDir dir;
	const auto file = dir.path / "Manifest.json";
	Manifest m;
	m.Load(file);
	m.SetContentKey("a.pso", "k");
	REQUIRE(m.Save());
	CHECK(fs::exists(file));

	Manifest other;
	other.Load(dir.path / "missing.json");
	CHECK_FALSE(other.GetContentKey("a.pso").has_value());
	m.Load(dir.path / "missing.json");
	CHECK_FALSE(m.GetContentKey("a.pso").has_value());
}

TEST_CASE("PruneIf removes a pruned entry's content key too", "[ShaderCacheManifest]")
{
	TempDir dir;
	Manifest m;
	m.Load(dir.path / "Manifest.json");
	m.Set("a.pso", "d");
	m.SetContentKey("a.pso", "k");
	m.Set("b.pso", "d2");
	m.SetContentKey("b.pso", "k2");
	CHECK(m.PruneIf([](const std::string& p) { return p == "a.pso"; }) == 1);
	CHECK_FALSE(m.GetContentKey("a.pso").has_value());
	CHECK(m.GetContentKey("b.pso") == "k2");
}
