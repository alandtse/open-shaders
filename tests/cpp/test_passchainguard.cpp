// Unit tests for the bounded cycle walker that guards SCM shadow renders against a cyclic
// BSRenderPass chain (src/Features/LightLimitFix/PassChainGuard.h).

#include "Features/LightLimitFix/PassChainGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using PassChainGuard::Verdict;
using PassChainGuard::Walk;

namespace
{
	struct Node
	{
		const Node* next = nullptr;
	};

	constexpr auto kNext = [](const Node* n) { return n->next; };

	// Builds a chain of `count` nodes; `loopTo` >= 0 links the last node back to that index.
	std::vector<Node> MakeChain(size_t count, int loopTo = -1)
	{
		std::vector<Node> nodes(count);
		for (size_t i = 0; i + 1 < count; ++i)
			nodes[i].next = &nodes[i + 1];
		if (loopTo >= 0 && count > 0)
			nodes[count - 1].next = &nodes[static_cast<size_t>(loopTo)];
		return nodes;
	}
}

TEST_CASE("Walk accepts empty and acyclic chains", "[llf][passguard]")
{
	REQUIRE(Walk<Node>(nullptr, kNext, 64) == Verdict::Clean);

	auto one = MakeChain(1);
	REQUIRE(Walk(&one[0], kNext, 64) == Verdict::Clean);

	auto hundred = MakeChain(100);
	uint32_t steps = 0;
	REQUIRE(Walk(&hundred[0], kNext, 4096, &steps) == Verdict::Clean);
	REQUIRE(steps == 100);
}

TEST_CASE("Walk detects self loops and short rings", "[llf][passguard]")
{
	auto self = MakeChain(1, 0);
	REQUIRE(Walk(&self[0], kNext, 64) == Verdict::Cycle);

	auto two = MakeChain(2, 0);
	REQUIRE(Walk(&two[0], kNext, 64) == Verdict::Cycle);
}

TEST_CASE("Walk detects the nine-pass ring seen in the SCM hang", "[llf][passguard]")
{
	auto ring = MakeChain(9, 0);
	REQUIRE(Walk(&ring[0], kNext, 8192) == Verdict::Cycle);
}

TEST_CASE("Walk detects a ring entered mid-chain (rho shape)", "[llf][passguard]")
{
	auto rho = MakeChain(14, 5);
	REQUIRE(Walk(&rho[0], kNext, 8192) == Verdict::Cycle);
}

TEST_CASE("Walk detects a ring from any entry node", "[llf][passguard]")
{
	auto ring = MakeChain(9, 0);
	for (auto& node : ring)
		REQUIRE(Walk(&node, kNext, 8192) == Verdict::Cycle);
}

TEST_CASE("Walk terminates at the cap and fails open on long chains", "[llf][passguard]")
{
	auto longChain = MakeChain(1000);
	uint32_t steps = 0;
	REQUIRE(Walk(&longChain[0], kNext, 100, &steps) == Verdict::CapExceeded);
	REQUIRE(steps <= 100);

	// A ring larger than the cap cannot be proven within it; the caller must fail open.
	auto bigRing = MakeChain(1000, 0);
	REQUIRE(Walk(&bigRing[0], kNext, 100) == Verdict::CapExceeded);

	// A legitimately long chain inside the cap is still accepted.
	auto legit = MakeChain(5000);
	REQUIRE(Walk(&legit[0], kNext, 8192) == Verdict::Clean);
}

using PassChainGuard::FindCycleClosingNode;

namespace
{
	// Severs the closing link and returns how many nodes are then reachable from `head`.
	uint32_t SeverAndCount(std::vector<Node>& nodes, const Node* closing)
	{
		const_cast<Node*>(closing)->next = nullptr;
		uint32_t steps = 0;
		REQUIRE(Walk(&nodes[0], kNext, 8192, &steps) == Verdict::Clean);
		return steps;
	}
}

TEST_CASE("FindCycleClosingNode returns null for acyclic chains", "[llf][passguard]")
{
	REQUIRE(FindCycleClosingNode<Node>(nullptr, kNext, 64) == nullptr);

	auto chain = MakeChain(50);
	REQUIRE(FindCycleClosingNode(&chain[0], kNext, 4096) == nullptr);
}

TEST_CASE("FindCycleClosingNode finds the node that closes the nine-pass ring", "[llf][passguard]")
{
	auto ring = MakeChain(9, 0);
	const Node* closing = FindCycleClosingNode(&ring[0], kNext, 8192);
	REQUIRE(closing == &ring[8]);
	REQUIRE(SeverAndCount(ring, closing) == 9);
}

TEST_CASE("FindCycleClosingNode repairs a tail leading into a ring", "[llf][passguard]")
{
	auto rho = MakeChain(14, 5);
	const Node* closing = FindCycleClosingNode(&rho[0], kNext, 8192);
	REQUIRE(closing == &rho[13]);
	REQUIRE(SeverAndCount(rho, closing) == 14);
}

TEST_CASE("FindCycleClosingNode handles a self loop", "[llf][passguard]")
{
	auto self = MakeChain(1, 0);
	const Node* closing = FindCycleClosingNode(&self[0], kNext, 64);
	REQUIRE(closing == &self[0]);
	REQUIRE(SeverAndCount(self, closing) == 1);
}

TEST_CASE("FindCycleClosingNode gives up beyond the cap", "[llf][passguard]")
{
	auto bigRing = MakeChain(1000, 0);
	REQUIRE(FindCycleClosingNode(&bigRing[0], kNext, 100) == nullptr);
}
