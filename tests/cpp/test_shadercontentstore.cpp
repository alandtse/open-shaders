// Unit tests for Util::ShaderContentStore (persistent compiled-shader store).
// Keys come from Util::CompileDedupe::MakeKey; a miss must read as empty,
// never as a usable blob.

#include "Utils/CompileDedupe.h"
#include "Utils/ShaderContentStore.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <process.h>
#include <thread>
#include <vector>

using namespace Util::ShaderContentStore;
using Util::CompileDedupe::KeyInputs;
using Util::CompileDedupe::MakeKey;
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

TEST_CASE("Put trims the store once it exceeds its cap", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path, 100);
	auto k1 = MakeKey(Base());
	auto in2 = Base();
	in2.flags = 1;
	auto k2 = MakeKey(in2);
	const std::string blob(60, 'x');
	REQUIRE(store.Put(k1, blob.data(), blob.size()));
	fs::last_write_time(store.PathFor(k1), fs::file_time_type::clock::now() - std::chrono::hours(1));
	REQUIRE(store.Put(k2, blob.data(), blob.size()));

	CHECK(store.Get(k1).empty());
	CHECK_FALSE(store.Get(k2).empty());
}

namespace
{
	std::string BinaryBlob(size_t a_size, unsigned a_seed)
	{
		std::string blob(a_size, '\0');
		for (size_t i = 0; i < a_size; ++i)
			blob[i] = static_cast<char>((i * 131 + a_seed) & 0xFF);
		return blob;
	}

	KeyInputs WithFlags(uint32_t a_flags)
	{
		auto in = Base();
		in.flags = a_flags;
		return in;
	}

	size_t CountFiles(const fs::path& a_root, std::string_view a_extension)
	{
		size_t count = 0;
		for (const auto& e : fs::recursive_directory_iterator(a_root))
			count += e.is_regular_file() && e.path().extension() == a_extension;
		return count;
	}

	/// Runs a_fn on a_threads threads released together; returns once all have finished.
	template <class Fn>
	void RunTogether(unsigned a_threads, Fn a_fn)
	{
		std::atomic<bool> go{ false };
		std::vector<std::thread> threads;
		for (unsigned i = 0; i < a_threads; ++i)
			threads.emplace_back([&, i] {
				while (!go.load(std::memory_order_acquire))
					std::this_thread::yield();
				a_fn(i);
			});
		go.store(true, std::memory_order_release);
		for (auto& t : threads)
			t.join();
	}
}

TEST_CASE("Binary blobs with embedded NULs round-trip exactly", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	const auto blob = BinaryBlob(70000, 7);
	REQUIRE(store.Put(MakeKey(Base()), blob.data(), blob.size()));
	const auto got = store.Get(MakeKey(Base()));
	REQUIRE(got.size() == blob.size());
	CHECK(std::equal(got.begin(), got.end(), blob.begin()));
}

TEST_CASE("Put leaves no temp files behind and overwrites in place", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	const auto key = MakeKey(Base());
	const auto blob = BinaryBlob(1024, 1);
	REQUIRE(store.Put(key, blob.data(), blob.size()));
	REQUIRE(store.Put(key, blob.data(), blob.size()));
	CHECK(CountFiles(dir.path, ".tmp") == 0);
	CHECK(CountFiles(dir.path, ".bin") == 1);
	CHECK(store.Get(key).size() == blob.size());
}

TEST_CASE("A zero-length entry on disk reads as a miss", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	const auto key = MakeKey(Base());
	fs::create_directories(store.PathFor(key).parent_path());
	std::ofstream(store.PathFor(key), std::ios::binary).close();
	CHECK(store.Get(key).empty());
}

TEST_CASE("Get refreshes recency so a read entry survives Trim", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	const auto k1 = MakeKey(WithFlags(1));
	const auto k2 = MakeKey(WithFlags(2));
	const std::string blob(100, 'x');
	REQUIRE(store.Put(k1, blob.data(), blob.size()));
	REQUIRE(store.Put(k2, blob.data(), blob.size()));
	const auto old = fs::file_time_type::clock::now() - std::chrono::hours(2);
	fs::last_write_time(store.PathFor(k1), old);
	fs::last_write_time(store.PathFor(k2), old - std::chrono::hours(1));

	REQUIRE_FALSE(store.Get(k1).empty());  // k1 was written first but is now the most recently used
	CHECK(store.Trim(150) == 1);
	CHECK_FALSE(store.Get(k1).empty());
	CHECK(store.Get(k2).empty());
}

TEST_CASE("Trim neither counts nor deletes files that are not blobs", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path);
	const auto stray = dir.path / "ab" / "abandoned.tmp";
	fs::create_directories(stray.parent_path());
	std::ofstream(stray, std::ios::binary) << std::string(5000, 'z');
	const std::string blob(10, 'x');
	REQUIRE(store.Put(MakeKey(Base()), blob.data(), blob.size()));

	CHECK(store.Trim(100) == 0);
	CHECK(fs::exists(stray));
}

TEST_CASE("Concurrent Put of the same key leaves one intact entry", "[ShaderContentStore][concurrency]")
{
	// Many compile threads finish identical shaders together; every round must end with the blob readable.
	TempDir dir;
	Store store(dir.path);
	constexpr unsigned kRounds = 40;
	constexpr unsigned kThreads = 16;
	std::atomic<unsigned> missingRounds{ 0 };
	std::atomic<unsigned> tornRounds{ 0 };
	for (unsigned round = 0; round < kRounds; ++round) {
		const auto key = MakeKey(WithFlags(round));
		const auto blob = BinaryBlob(200000, round);
		RunTogether(kThreads, [&](unsigned) { store.Put(key, blob.data(), blob.size()); });
		const auto got = store.Get(key);
		if (got.empty())
			++missingRounds;
		else if (got.size() != blob.size() || !std::equal(got.begin(), got.end(), blob.begin()))
			++tornRounds;
	}
	CHECK(tornRounds == 0);
	CHECK(missingRounds == 0);
	CHECK(CountFiles(dir.path, ".tmp") == 0);
}

TEST_CASE("Concurrent Put of distinct keys keeps every entry", "[ShaderContentStore][concurrency]")
{
	TempDir dir;
	Store store(dir.path);
	constexpr unsigned kThreads = 24;
	RunTogether(kThreads, [&](unsigned i) {
		const auto blob = BinaryBlob(50000, i);
		store.Put(MakeKey(WithFlags(i)), blob.data(), blob.size());
	});
	for (unsigned i = 0; i < kThreads; ++i) {
		const auto blob = BinaryBlob(50000, i);
		const auto got = store.Get(MakeKey(WithFlags(i)));
		REQUIRE(got.size() == blob.size());
		CHECK(std::equal(got.begin(), got.end(), blob.begin()));
	}
}

TEST_CASE("Readers never see a torn blob while writers and Trim run", "[ShaderContentStore][concurrency]")
{
	TempDir dir;
	Store store(dir.path, 3 * 100000);  // small cap so Put keeps triggering Trim
	constexpr unsigned kKeys = 8;
	std::vector<std::string> blobs;
	for (unsigned i = 0; i < kKeys; ++i)
		blobs.push_back(BinaryBlob(100000, i));
	std::atomic<unsigned> torn{ 0 };
	std::atomic<unsigned> hits{ 0 };
	RunTogether(12, [&](unsigned t) {
		for (unsigned n = 0; n < 150; ++n) {
			const unsigned k = (t + n) % kKeys;
			const auto key = MakeKey(WithFlags(k));
			if (t % 3 == 0) {
				store.Put(key, blobs[k].data(), blobs[k].size());
			} else {
				const auto got = store.Get(key);
				if (got.empty())
					continue;
				++hits;
				if (got.size() != blobs[k].size() || !std::equal(got.begin(), got.end(), blobs[k].begin()))
					++torn;
			}
		}
	});
	CHECK(torn == 0);
	CHECK(hits > 0);
}

TEST_CASE("Clear removes every blob and later Puts recreate the store", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path / "store");
	const std::vector<char> blob{ 'D', 'X', 'B', 'C' };
	const auto key = MakeKey(Base());
	REQUIRE(store.Put(key, blob.data(), blob.size()));
	store.Clear();
	CHECK(store.Get(key).empty());
	REQUIRE(store.Put(key, blob.data(), blob.size()));
	CHECK(store.Get(key) == blob);
}

TEST_CASE("MeasureUsage totals blobs and ignores other files", "[ShaderContentStore]")
{
	TempDir dir;
	const auto root = dir.path / "store";
	Store store(root);
	const std::vector<char> blob(100, 'x');
	REQUIRE(store.Put(MakeKey(Base()), blob.data(), blob.size()));
	std::ofstream(root / "stray.txt") << "not a blob";

	const auto usage = MeasureUsage(root);
	CHECK(usage.blobs == 1);
	CHECK(usage.bytes == blob.size());
	CHECK(MeasureUsage(dir.path / "missing").blobs == 0);
}

TEST_CASE("Lowering the cap with SetMaxBytes lets Trim evict down to it", "[ShaderContentStore]")
{
	TempDir dir;
	Store store(dir.path / "store", 10'000);
	const std::vector<char> blob(100, 'x');
	for (unsigned i = 0; i < 20; ++i)
		REQUIRE(store.Put(MakeKey({ std::format("code {}", i), "main", "ps_5_0", 0, "c" }), blob.data(), blob.size()));
	REQUIRE(MeasureUsage(dir.path / "store").blobs == 20);

	store.SetMaxBytes(500);
	store.Trim(500);
	CHECK(MeasureUsage(dir.path / "store").bytes <= 500);
	CHECK(MeasureUsage(dir.path / "store").blobs >= 1);
}
