#include "EngineFixes/AlphaPassListPolicy.h"
#include "Features/LightLimitFix/PassChainGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace
{
	using namespace AlphaPassList;

	struct NativeQueue
	{
		Queue state{ .group = 1, .flags = 1 };
		std::unordered_map<std::uintptr_t, std::uintptr_t> links;
		std::size_t reads{}, forwards{};

		bool Read(std::uintptr_t node, std::uintptr_t& next)
		{
			++reads;
			const auto found = links.find(node);
			if (found == links.end())
				return false;
			next = found->second;
			return true;
		}

		void Append(std::uintptr_t pass, bool front = false)
		{
			++forwards;
			const auto previousHead = state.head;
			if (!state.head)
				state.head = state.tail = pass;
			else if (front)
				state.head = pass;
			else {
				links[state.tail] = pass;
				state.tail = pass;
			}
			links[pass] = front ? previousHead : 0;
			++state.count;
		}

		template <class MembershipCache>
		AppendResult Guard(MembershipCache* cache, std::uintptr_t pass, bool front = false, std::size_t* failures = nullptr)
		{
			return AppendUnique(cache, state, pass, front, [&](auto node, auto& next) { return Read(node, next); }, [&] { Append(pass, front); }, [&] { return state; }, [&] { if (failures) ++*failures; });
		}

		std::vector<std::uintptr_t> Walk() const
		{
			std::vector<std::uintptr_t> result;
			auto node = state.head;
			while (node && result.size() < MaximumWalkNodes) {
				result.push_back(node);
				node = links.at(node);
			}
			return result;
		}
	};

	struct AllocationState
	{
		static inline thread_local std::size_t count{};
		static inline thread_local std::size_t liveAllocations{};
		static inline thread_local std::ptrdiff_t failAfter{ -1 };
	};

	template <class T>
	struct TestAllocator
	{
		using value_type = T;
		using is_always_equal = std::true_type;
		TestAllocator() = default;
		template <class U>
		TestAllocator(const TestAllocator<U>&) noexcept
		{}

		T* allocate(std::size_t count)
		{
			if (AllocationState::failAfter >= 0 && AllocationState::failAfter-- == 0)
				throw std::bad_alloc();
			++AllocationState::count;
			auto* memory = std::allocator<T>{}.allocate(count);
			++AllocationState::liveAllocations;
			return memory;
		}

		void deallocate(T* memory, std::size_t count) noexcept
		{
			--AllocationState::liveAllocations;
			std::allocator<T>{}.deallocate(memory, count);
		}
		bool operator==(const TestAllocator&) const = default;
	};

	using AllocationCache = MembershipCache<MaximumCachedPasses, TestAllocator<std::uintptr_t>>;

	struct AllocationFailure
	{
		explicit AllocationFailure(std::ptrdiff_t after) { AllocationState::failAfter = after; }
		~AllocationFailure() { AllocationState::failAfter = -1; }
	};

	struct FailingCache
	{
		bool check{}, commit{};
		std::size_t invalidations{};

		Membership Check(const Queue&, std::uintptr_t, auto&)
		{
			if (check)
				throw std::bad_alloc();
			return Membership::absent;
		}

		void Commit(const Queue&, const Queue&, std::uintptr_t, bool)
		{
			if (commit)
				throw std::bad_alloc();
		}

		void Invalidate() noexcept { ++invalidations; }
	};
}

TEST_CASE("Alpha membership prevents four native queue truncation traces", "[vr][alpha-list]")
{
	struct Capture
	{
		std::uintptr_t calls, duplicateIndex;
		std::size_t reachable;
	};
	for (const auto capture : { Capture{ 226, 143, 102 }, Capture{ 207, 122, 104 }, Capture{ 210, 163, 66 }, Capture{ 203, 158, 64 } }) {
		CAPTURE(capture.calls, capture.duplicateIndex, capture.reachable);
		NativeQueue native, fixed;
		Cache cache;
		for (std::uintptr_t i = 0; i < capture.calls; ++i) {
			const auto pass = i == capture.duplicateIndex ? 20 : i + 1;
			native.Append(pass);
			fixed.Guard(&cache, pass);
		}
		REQUIRE(native.state.count == capture.calls);
		REQUIRE(native.Walk().size() == capture.reachable);
		CHECK(Contains(native.state.head, 61, [&](auto node, auto& next) { return native.Read(node, next); }) == Membership::absent);
		REQUIRE(fixed.state.count == capture.calls - 1);
		REQUIRE(fixed.Walk().size() == capture.calls - 1);
		CHECK(fixed.forwards == capture.calls - 1);
		CHECK(Contains(fixed.state.head, 61, [&](auto node, auto& next) { return fixed.Read(node, next); }) == Membership::present);
		CHECK(fixed.reads <= 61);

		const auto next = [&](const void* node) -> const void* {
			return reinterpret_cast<const void*>(native.links.at(reinterpret_cast<std::uintptr_t>(node)));
		};
		const auto* head = reinterpret_cast<const void*>(native.state.head);
		std::uint32_t reachable{};
		CHECK(PassChainGuard::Walk(head, next, MaximumWalkNodes, &reachable) == PassChainGuard::Verdict::Clean);
		CHECK(reachable == capture.reachable);
		CHECK(PassChainGuard::FindCycleClosingNode(head, next, MaximumWalkNodes) == nullptr);
	}
}

TEST_CASE("Alpha membership preserves unique front and back insertions", "[vr][alpha-list]")
{
	NativeQueue queue;
	Cache cache;
	CHECK(queue.Guard(&cache, 1).forwarded);
	CHECK(queue.Guard(&cache, 2).forwarded);
	CHECK_FALSE(queue.Guard(&cache, 1, true).forwarded);
	CHECK_FALSE(queue.Guard(&cache, 2).forwarded);
	CHECK(queue.Guard(&cache, 3, true).forwarded);
	CHECK(queue.Walk() == std::vector<std::uintptr_t>{ 3, 1, 2 });
	CHECK(queue.state.count == 3);
}

TEST_CASE("Alpha membership reseeds when queue ownership changes", "[vr][alpha-list]")
{
	NativeQueue queue;
	Cache first, second;
	queue.Guard(&first, 1);
	queue.Guard(&first, 2);
	CHECK_FALSE(queue.Guard(&second, 2).forwarded);
	CHECK(queue.Guard(&second, 3).forwarded);
	CHECK_FALSE(queue.Guard(&first, 3).forwarded);
	CHECK(queue.Walk() == std::vector<std::uintptr_t>{ 1, 2, 3 });
}

TEST_CASE("Alpha frame and generation changes preserve still-pending membership", "[vr][alpha-list]")
{
	NativeQueue queue;
	Cache cache;
	queue.Guard(&cache, 1);
	++queue.state.frame;
	CHECK_FALSE(queue.Guard(&cache, 1).forwarded);
	++queue.state.generation;
	CHECK_FALSE(queue.Guard(&cache, 1).forwarded);
	queue.state.head = queue.state.tail = queue.state.count = 0;
	CHECK(queue.Guard(&cache, 1).forwarded);
	CHECK(queue.state.count == 1);
}

TEST_CASE("Alpha eviction reseeds pending queues", "[vr][alpha-list]")
{
	NativeQueue queue;
	Cache cache;
	queue.Guard(&cache, 1);
	for (std::uintptr_t group = 2; group < MaximumCachedGroups + 3; ++group) {
		NativeQueue other;
		other.state.group = group;
		REQUIRE(other.Guard(&cache, group * 10).forwarded);
	}
	CHECK_FALSE(queue.Guard(&cache, 1).forwarded);
	CHECK(queue.Walk() == std::vector<std::uintptr_t>{ 1 });
}

TEST_CASE("Alpha retirement rejects stale same-frame ABA membership", "[vr][alpha-list]")
{
	NativeQueue queue;
	Cache cache;
	for (const auto pass : { 1, 2, 3 })
		queue.Guard(&cache, pass);
	queue.state.head = queue.state.tail = queue.state.count = 0;
	++queue.state.generation;
	for (const auto pass : { 1, 4, 3 })
		queue.Append(pass);
	CHECK_FALSE(queue.Guard(&cache, 4).forwarded);
	CHECK(queue.Guard(&cache, 2).forwarded);
	CHECK(queue.Walk() == std::vector<std::uintptr_t>{ 1, 4, 3, 2 });
}

TEST_CASE("Alpha policy matches unique native insertion across interleaved queues", "[vr][alpha-list]")
{
	std::array<NativeQueue, 8> queues;
	std::array<std::vector<std::uintptr_t>, 8> expected;
	Cache first, second;
	std::uint32_t random = 0x19A17E;
	for (std::size_t i = 0; i < queues.size(); ++i)
		queues[i].state.group = i + 1;
	for (std::size_t step = 0; step < 10000; ++step) {
		CAPTURE(step);
		random = random * 1664525u + 1013904223u;
		const auto index = (random >> 8) % queues.size();
		auto& queue = queues[index];
		auto& model = expected[index];
		const auto operation = (random >> 12) % 16;
		if (operation == 0) {
			model.clear();
			queue.state.head = queue.state.tail = queue.state.count = 0;
			++queue.state.generation;
		} else if (operation == 1 && !model.empty()) {
			queue.state.head = queue.links.at(queue.state.head);
			if (!queue.state.head)
				queue.state.tail = 0;
			--queue.state.count;
			model.erase(model.begin());
			++queue.state.generation;
		} else if (operation == 2) {
			++queue.state.frame;
		} else {
			const auto pass = index * 100 + ((random >> 16) % 64) + 1;
			const bool front = (random & 16) != 0;
			const bool pending = std::find(model.begin(), model.end(), pass) != model.end();
			const auto result = queue.Guard((random & 1) ? &first : &second, pass, front);
			REQUIRE(result.forwarded != pending);
			REQUIRE(result.membershipKnown);
			if (!pending)
				model.insert(front ? model.begin() : model.end(), pass);
		}
		REQUIRE(queue.Walk() == model);
		REQUIRE(queue.state.count == model.size());
	}
}

TEST_CASE("Alpha warm appends reuse allocations without scanning native links", "[vr][alpha-list]")
{
	NativeQueue queue;
	AllocationCache cache;
	for (std::uintptr_t pass = 1; pass <= 512; ++pass)
		queue.Guard(&cache, pass);
	queue.state.head = queue.state.tail = queue.state.count = 0;
	++queue.state.frame;
	++queue.state.generation;
	queue.reads = 0;
	const auto before = AllocationState::count;
	for (std::uintptr_t pass = 1; pass <= 512; ++pass)
		queue.Guard(&cache, pass);
	CHECK(AllocationState::count == before);
	CHECK(queue.reads == 0);
	CHECK(queue.state.count == 512);
}

TEST_CASE("Alpha real allocator failures do not drop or replay native appends", "[vr][alpha-list]")
{
	for (const bool pending : { false, true }) {
		for (std::ptrdiff_t failure = 0; failure < 5; ++failure) {
			CAPTURE(pending, failure);
			const auto liveBefore = AllocationState::liveAllocations;
			{
				NativeQueue queue;
				if (pending)
					queue.Append(1);
				AllocationCache cache;
				std::size_t failures{};
				AppendResult result;
				{
					AllocationFailure injection(failure);
					result = queue.Guard(&cache, 2, false, &failures);
				}
				CHECK(result.forwarded);
				CHECK(queue.forwards == (pending ? 2u : 1u));
				if (failure == 0)
					CHECK(failures == 1);
				CHECK_FALSE(queue.Guard(&cache, 2).forwarded);
				CHECK(queue.Walk().size() == queue.forwards);
			}
			CHECK(AllocationState::liveAllocations == liveBefore);
		}
	}
}

TEST_CASE("Alpha capacity fallback finds uncached pending passes", "[vr][alpha-list]")
{
	NativeQueue queue;
	constexpr std::size_t limit = 8;
	MembershipCache<limit> cache;
	for (std::uintptr_t pass = 1; pass <= limit + 3; ++pass)
		queue.Guard(&cache, pass);
	CHECK_FALSE(queue.Guard(&cache, limit + 2).forwarded);
	CHECK(queue.Walk().size() == limit + 3);
}

TEST_CASE("Alpha incomplete native walks cannot prove absence", "[vr][alpha-list]")
{
	std::size_t reads{};
	const auto unreadable = [&](auto, auto&) { ++reads; return false; };
	CHECK(Contains(100, 101, unreadable) == Membership::unknown);
	CHECK(reads == 1);
	reads = 0;
	const auto cycle = [&](auto, auto& next) { ++reads; next = 100; return true; };
	CHECK(Contains(100, 101, cycle) == Membership::unknown);
	CHECK(reads == MaximumWalkNodes);
	CHECK(Contains(100, 100, unreadable) == Membership::present);
}

TEST_CASE("Alpha unverifiable membership forwards exactly once", "[vr][alpha-list]")
{
	for (const bool cached : { false, true }) {
		Cache cache;
		Queue queue{ .group = 1, .head = 100, .tail = 100, .count = 1, .flags = 1 };
		std::size_t forwarded{};
		const auto result = AppendUnique(cached ? &cache : nullptr, queue, 101, false, [](auto, auto&) { return false; }, [&] { ++forwarded; }, [&] { return queue; }, [] {});
		CHECK(result.forwarded);
		CHECK_FALSE(result.membershipKnown);
		CHECK(forwarded == 1);
	}
}

TEST_CASE("Alpha cache-check and cache-commit failures preserve call ownership", "[vr][alpha-list]")
{
	NativeQueue queue;
	std::size_t failures{};
	FailingCache before{ true, false };
	CHECK(queue.Guard(&before, 1, false, &failures).forwarded);
	CHECK(queue.forwards == 1);
	CHECK(before.invalidations == 1);
	CHECK(failures == 1);
	FailingCache after{ false, true };
	CHECK(queue.Guard(&after, 2, false, &failures).forwarded);
	CHECK(queue.forwards == 2);
	CHECK(after.invalidations == 1);
	CHECK(failures == 2);
	CHECK_FALSE(queue.Guard(static_cast<Cache*>(nullptr), 1).forwarded);
}
