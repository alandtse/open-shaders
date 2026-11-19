#pragma once

#include <ankerl/unordered_dense.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace AlphaPassList
{
	inline constexpr std::size_t MaximumCachedGroups = 32;
	inline constexpr std::size_t MaximumCachedPasses = 65536;
	inline constexpr std::size_t MaximumWalkNodes = 65536;
	enum class Membership
	{
		absent,
		present,
		unknown
	};
	struct AppendResult
	{
		bool forwarded{}, membershipKnown{};
	};
	struct Queue
	{
		std::uintptr_t group{}, head{}, tail{};
		std::uint64_t generation{};
		std::uint32_t frame{};
		std::uint16_t count{};
		std::uint8_t flags{};
		bool operator==(const Queue&) const = default;
	};
	/** Incomplete or unreadable walks never prove absence. */
	template <class ReadNext>
	Membership Contains(std::uintptr_t head, std::uintptr_t pass, ReadNext&& read)
	{
		for (std::size_t i = 0; head && i < MaximumWalkNodes; ++i) {
			if (head == pass)
				return Membership::present;
			if (!read(head, head))
				return Membership::unknown;
		}
		return head ? Membership::unknown : Membership::absent;
	}
	/** Native callers serialize a queue; thread handoffs reseed from its current links. */
	template <std::size_t PassLimit = MaximumCachedPasses, class Allocator = std::allocator<std::uintptr_t>>
	class MembershipCache
	{
		static_assert(PassLimit > 0 && PassLimit <= MaximumWalkNodes);
		struct Entry
		{
			Queue queue{};
			ankerl::unordered_dense::set<std::uintptr_t, ankerl::unordered_dense::hash<std::uintptr_t>, std::equal_to<std::uintptr_t>, Allocator> passes;
			std::uint64_t use{};
			bool complete{};
		};
		std::array<Entry, MaximumCachedGroups> entries;
		Entry* current{};
		std::uint64_t use{};

		Entry& Find(std::uintptr_t group)
		{
			if (current && current->queue.group == group) {
				current->use = ++use;
				return *current;
			}
			auto* oldest = &entries[0];
			for (auto& entry : entries) {
				if (entry.queue.group == group) {
					entry.use = ++use;
					current = &entry;
					return entry;
				}
				if (entry.use < oldest->use)
					oldest = &entry;
			}
			oldest->queue = {};
			oldest->passes.clear();
			oldest->complete = false;
			oldest->use = ++use;
			current = oldest;
			return *oldest;
		}

	public:
		MembershipCache() = default;
		MembershipCache(const MembershipCache&) = delete;
		MembershipCache& operator=(const MembershipCache&) = delete;
		void Invalidate() noexcept
		{
			current = nullptr;
			for (auto& entry : entries) {
				entry.queue = {};
				entry.complete = false;
			}
		}
		/** Reuse buckets across queues/frames without retaining native object ownership. */
		template <class ReadNext>
		Membership Check(const Queue& queue, std::uintptr_t pass, ReadNext&& read)
		{
			auto& entry = Find(queue.group);
			if (entry.queue != queue) {
				entry.queue = {};
				entry.passes.clear();
				entry.complete = true;
				auto node = queue.head;
				std::size_t visited{};
				for (; node && visited < MaximumWalkNodes; ++visited) {
					if (entry.passes.contains(node)) {
						entry.complete = false;
						break;
					}
					if (entry.passes.size() < PassLimit)
						entry.passes.insert(node);
					else
						entry.complete = false;
					if (!read(node, node)) {
						entry.complete = false;
						break;
					}
				}
				entry.complete &= node == 0;
				entry.queue = queue;
			}
			if (entry.passes.contains(pass))
				return Membership::present;
			return entry.complete ? Membership::absent : Contains(queue.head, pass, read);
		}
		/** Publish cache membership only after the expected native append completed. */
		void Commit(const Queue& before, const Queue& after, std::uintptr_t pass, bool front)
		{
			auto expected = before;
			expected.count = static_cast<std::uint16_t>(before.count + 1);
			if (!before.head)
				expected.head = expected.tail = pass;
			else if (front)
				expected.head = pass;
			else
				expected.tail = pass;
			auto& entry = Find(before.group);
			if (after != expected || entry.queue != before) {
				entry.queue = {};
				entry.complete = false;
				return;
			}
			if (entry.passes.size() < PassLimit)
				entry.passes.insert(pass);
			else
				entry.complete = false;
			entry.queue = after;
		}
	};
	using Cache = MembershipCache<>;
	/** Cache failure falls back to a bounded walk; never replay an already forwarded call. */
	template <class MembershipCache, class ReadNext, class Forward, class Inspect, class ReportFailure>
	AppendResult AppendUnique(MembershipCache* cache, Queue before, std::uintptr_t pass, bool front,
		ReadNext&& read, Forward&& forward, Inspect&& inspect, ReportFailure&& report)
	{
		Membership membership = Membership::unknown;
		if (cache) {
			try {
				membership = cache->Check(before, pass, read);
			} catch (const std::bad_alloc&) {
				cache->Invalidate();
				report();
				cache = nullptr;
			}
		}
		if (!cache)
			membership = Contains(before.head, pass, read);
		if (membership == Membership::present)
			return { false, true };
		forward();
		if (cache) {
			try {
				cache->Commit(before, inspect(), pass, front);
			} catch (const std::bad_alloc&) {
				cache->Invalidate();
				report();
			}
		}
		return { true, membership != Membership::unknown };
	}
}
