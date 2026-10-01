// Unit tests for Util::ShaderContentStore (content-addressed compiled-shader
// store). A key must change with any input that changes bytecode, and a miss
// must read as empty, never as a usable blob.

#include "Utils/ShaderContentStore.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <process.h>

using namespace Util::ShaderContentStore;
namespace fs = std::filesystem;

namespace
{
	struct TempDir
	{
		fs::path path;
		TempDir()
		{
			static std::atomic<unsigned> counter{ 0 };
			path = fs::temp_directory_path() / std::format("cscs_{}_{}_{}", ::_getpid(),
												   std::chrono::high_resolution_clock::now().time_since_epoch().count(), counter++);
			fs::create_directories(path);
		}
		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	};

	KeyInputs Base()
	{
		return { "float4 main() : SV_Target { return 0; }", "main", "ps_5_0", 0x8000, "d3dcompiler_47" };
	}
}

TEST_CASE("Key is stable for identical inputs", "[ShaderContentStore]")
{
	CHECK(MakeKey(Base()) == MakeKey(Base()));
}

TEST_CASE("Key changes with every bytecode-affecting input", "[ShaderContentStore]")
{
	const auto base = MakeKey(Base());

	auto a = Base();
	a.preprocessed = "float4 main() : SV_Target { return 1; }";
	CHECK_FALSE(MakeKey(a) == base);

	auto b = Base();
	b.entryPoint = "other";
	CHECK_FALSE(MakeKey(b) == base);

	auto c = Base();
	c.profile = "ps_5_1";
	CHECK_FALSE(MakeKey(c) == base);

	auto d = Base();
	d.flags = 0xC000;
	CHECK_FALSE(MakeKey(d) == base);

	auto e = Base();
	e.compilerId = "d3dcompiler_48";
	CHECK_FALSE(MakeKey(e) == base);
}

TEST_CASE("Get on a missing entry is empty", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	CHECK(store.Get(MakeKey(Base())).empty());
}

TEST_CASE("Put then Get round-trips a blob", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	const auto key = MakeKey(Base());
	const std::string blob = "DXBC-bytes";
	REQUIRE(store.Put(key, blob.data(), blob.size()));
	const auto got = store.Get(key);
	CHECK(std::string(got.begin(), got.end()) == blob);
}

TEST_CASE("Put rejects empty data", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	CHECK_FALSE(store.Put(MakeKey(Base()), nullptr, 0));
}

TEST_CASE("Trim evicts least recently used entries first", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	auto k1 = MakeKey(Base());
	auto in2 = Base();
	in2.flags = 1;
	auto k2 = MakeKey(in2);
	const std::string blob(100, 'x');
	REQUIRE(store.Put(k1, blob.data(), blob.size()));
	REQUIRE(store.Put(k2, blob.data(), blob.size()));
	fs::last_write_time(store.PathFor(k1), fs::file_time_type::clock::now() - std::chrono::hours(1));

	CHECK(store.Trim(150) == 1);
	CHECK(store.Get(k1).empty());
	CHECK_FALSE(store.Get(k2).empty());
}

TEST_CASE("Trim under the limit removes nothing", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	const std::string blob(10, 'x');
	REQUIRE(store.Put(MakeKey(Base()), blob.data(), blob.size()));
	CHECK(store.Trim(1000) == 0);
}

TEST_CASE("StripLineDirectives drops only #line lines", "[ShaderContentStore]")
{
	const std::string in = "#line 1 \"C:\\a\\b.hlsl\"\nfloat x;\n#line 7\r\nfloat y;";
	CHECK(StripLineDirectives(in) == "float x;\nfloat y;");
	CHECK(StripLineDirectives("") == "");
}
