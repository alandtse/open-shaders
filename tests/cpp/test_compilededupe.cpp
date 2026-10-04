// Unit tests for Util::CompileDedupe (shares one shader compile between tasks whose
// bytecode inputs are identical). Exactly one task may compile a key at a time, waiters
// must receive the owner's bytes, and a failed owner must hand the key to a retry.

#include "Utils/CompileDedupe.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace Util::CompileDedupe;
using Util::ContentHash::Hash128;

namespace
{
	KeyInputs Base()
	{
		return { "float4 main() : SV_Target { return 0; }", "main", "ps_5_0", 0x8000, "d3dcompiler_47" };
	}

	Hash128 KeyN(uint64_t a_n)
	{
		return Hash128{ a_n, ~a_n };
	}

	std::vector<char> Blob(size_t a_size, unsigned a_seed)
	{
		std::vector<char> blob(a_size);
		for (size_t i = 0; i < a_size; ++i)
			blob[i] = static_cast<char>((i * 131 + a_seed) & 0xFF);
		return blob;
	}

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

TEST_CASE("Key is stable and changes with every bytecode-affecting input", "[CompileDedupe]")
{
	const auto base = MakeKey(Base());
	CHECK(MakeKey(Base()) == base);

	auto a = Base();
	a.preprocessed = "float4 main() : SV_Target { return 1; }";
	auto b = Base();
	b.entryPoint = "other";
	auto c = Base();
	c.profile = "vs_5_0";
	auto d = Base();
	d.flags = 0x8001;
	auto e = Base();
	e.compilerId = "other";
	for (const auto& in : { a, b, c, d, e })
		CHECK_FALSE(MakeKey(in) == base);
}

TEST_CASE("Key depends on which field text lives in", "[CompileDedupe]")
{
	const KeyInputs a{ "ab", "c", "ps_5_0", 0, "x" };
	const KeyInputs b{ "a", "bc", "ps_5_0", 0, "x" };
	CHECK_FALSE(MakeKey(a) == MakeKey(b));
}

TEST_CASE("StripLineDirectives drops only #line lines", "[CompileDedupe]")
{
	CHECK(StripLineDirectives("#line 1 \"C:/a/b.hlsl\"\nfloat x;\n#line 7\r\nfloat y;") == "float x;\nfloat y;");
	CHECK(StripLineDirectives("#pragma line\nx = 1; // #line 3\n") == "#pragma line\nx = 1; // #line 3\n");
	CHECK(StripLineDirectives("") == "");
}

TEST_CASE("First acquire owns the key and a finished blob is shared", "[CompileDedupe]")
{
	Registry registry;
	auto first = registry.Acquire(KeyN(1));
	REQUIRE(first.ticket.has_value());
	CHECK_FALSE(first.blob);

	const auto blob = Blob(1000, 3);
	first.ticket->Publish(blob.data(), blob.size());

	auto second = registry.Acquire(KeyN(1));
	REQUIRE(second.blob);
	CHECK_FALSE(second.ticket.has_value());
	CHECK(*second.blob == blob);
}

TEST_CASE("Distinct keys each get an owner", "[CompileDedupe]")
{
	Registry registry;
	auto a = registry.Acquire(KeyN(1));
	auto b = registry.Acquire(KeyN(2));
	CHECK(a.ticket.has_value());
	CHECK(b.ticket.has_value());
}

TEST_CASE("Concurrent acquirers of one key produce exactly one owner and share its bytes", "[CompileDedupe][concurrency]")
{
	constexpr unsigned kRounds = 30;
	constexpr unsigned kThreads = 16;
	std::atomic<unsigned> badRounds{ 0 };
	for (unsigned round = 0; round < kRounds; ++round) {
		Registry registry;
		const auto blob = Blob(50000, round);
		std::atomic<unsigned> owners{ 0 };
		std::atomic<unsigned> shared{ 0 };
		std::atomic<unsigned> wrong{ 0 };
		RunTogether(kThreads, [&](unsigned) {
			auto acquired = registry.Acquire(KeyN(round));
			if (acquired.ticket) {
				++owners;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				acquired.ticket->Publish(blob.data(), blob.size());
			} else if (acquired.blob) {
				++shared;
				if (*acquired.blob != blob)
					++wrong;
			}
		});
		if (owners != 1 || shared != kThreads - 1 || wrong != 0)
			++badRounds;
	}
	CHECK(badRounds == 0);
}

TEST_CASE("Waiters block until the owner publishes", "[CompileDedupe][concurrency]")
{
	Registry registry;
	auto owner = registry.Acquire(KeyN(7));
	REQUIRE(owner.ticket.has_value());
	std::atomic<bool> published{ false };
	std::atomic<bool> sawEarly{ false };
	std::thread waiter([&] {
		auto acquired = registry.Acquire(KeyN(7));
		if (!published.load(std::memory_order_acquire))
			sawEarly = true;
		CHECK(acquired.blob);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const auto blob = Blob(10, 1);
	published.store(true, std::memory_order_release);
	owner.ticket->Publish(blob.data(), blob.size());
	waiter.join();
	CHECK_FALSE(sawEarly);
}

TEST_CASE("A failed owner hands the key to exactly one retry", "[CompileDedupe][concurrency]")
{
	Registry registry;
	auto owner = registry.Acquire(KeyN(9));
	REQUIRE(owner.ticket.has_value());
	const auto blob = Blob(2000, 5);
	std::atomic<unsigned> newOwners{ 0 };
	std::atomic<unsigned> shared{ 0 };
	std::vector<std::thread> waiters;
	for (unsigned i = 0; i < 6; ++i)
		waiters.emplace_back([&] {
			auto acquired = registry.Acquire(KeyN(9));
			if (acquired.ticket) {
				++newOwners;
				acquired.ticket->Publish(blob.data(), blob.size());
			} else if (acquired.blob && *acquired.blob == blob) {
				++shared;
			}
		});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	owner.ticket.reset();  // dropped unpublished: the claim fails
	for (auto& t : waiters)
		t.join();
	CHECK(newOwners == 1);
	CHECK(shared == 5);
}

TEST_CASE("Blobs over the retention cap reach current waiters but are not kept", "[CompileDedupe]")
{
	Registry registry(100);
	auto owner = registry.Acquire(KeyN(1));
	REQUIRE(owner.ticket.has_value());
	const auto big = Blob(500, 2);
	owner.ticket->Publish(big.data(), big.size());
	CHECK(registry.RetainedBytes() == 0);
	CHECK(registry.DroppedBlobs() == 1);

	auto later = registry.Acquire(KeyN(1));
	CHECK(later.ticket.has_value());
}

TEST_CASE("Retained blobs are counted and Clear releases them", "[CompileDedupe]")
{
	Registry registry;
	auto owner = registry.Acquire(KeyN(1));
	const auto blob = Blob(400, 2);
	owner.ticket->Publish(blob.data(), blob.size());
	CHECK(registry.RetainedBytes() == 400);

	registry.Clear();
	CHECK(registry.RetainedBytes() == 0);
	auto again = registry.Acquire(KeyN(1));
	CHECK(again.ticket.has_value());
}

TEST_CASE("Clear while a claim is pending does not strand its owner or waiters", "[CompileDedupe][concurrency]")
{
	Registry registry;
	auto owner = registry.Acquire(KeyN(4));
	REQUIRE(owner.ticket.has_value());
	std::atomic<bool> gotBlob{ false };
	std::thread waiter([&] { gotBlob = registry.Acquire(KeyN(4)).blob != nullptr; });
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	registry.Clear();
	const auto blob = Blob(64, 8);
	owner.ticket->Publish(blob.data(), blob.size());
	waiter.join();
	CHECK(gotBlob);
}

TEST_CASE("A publish whose claim was cleared reaches its waiters without being charged", "[CompileDedupe][concurrency]")
{
	Registry registry(100);
	auto owner = registry.Acquire(KeyN(5));
	REQUIRE(owner.ticket.has_value());
	std::atomic<bool> gotBlob{ false };
	std::thread waiter([&] { gotBlob = registry.Acquire(KeyN(5)).blob != nullptr; });
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	registry.Clear();
	const auto blob = Blob(500, 3);
	owner.ticket->Publish(blob.data(), blob.size());
	waiter.join();
	CHECK(gotBlob);
	CHECK(registry.RetainedBytes() == 0);
	CHECK(registry.DroppedBlobs() == 0);
}

TEST_CASE("Binary blobs with embedded NULs round-trip exactly", "[CompileDedupe]")
{
	Registry registry;
	auto owner = registry.Acquire(KeyN(1));
	const auto blob = Blob(70000, 7);
	owner.ticket->Publish(blob.data(), blob.size());
	auto got = registry.Acquire(KeyN(1));
	REQUIRE(got.blob);
	CHECK(*got.blob == blob);
}
