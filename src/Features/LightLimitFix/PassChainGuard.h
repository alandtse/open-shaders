#pragma once

#include <cstdint>

/// Bounded cycle detection over a singly linked chain, so a cyclic BSRenderPass chain can be
/// refused before the engine's walk of it never returns.
namespace PassChainGuard
{
	enum class Verdict : uint8_t
	{
		Clean,        ///< chain ends in null
		Cycle,        ///< chain loops back on itself
		CapExceeded,  ///< neither proven within the step cap; callers must fail open
	};

	/**
	 * @brief Brent's cycle detection, terminating on null, a repeated node, or `cap` steps.
	 * @param head First node, may be null.
	 * @param nextOf Callable returning the successor of a node (null ends the chain).
	 * @param cap Maximum successor reads before giving up.
	 * @param stepsOut Optional receiver for the number of successor reads performed.
	 */
	template <typename Node, typename NextFn>
	Verdict Walk(const Node* head, NextFn&& nextOf, uint32_t cap, uint32_t* stepsOut = nullptr)
	{
		uint32_t steps = 0;
		Verdict verdict = Verdict::Clean;
		if (head) {
			const Node* tortoise = head;
			const Node* hare = nextOf(head);
			++steps;
			uint32_t power = 1;
			uint32_t lambda = 1;
			while (hare) {
				if (hare == tortoise) {
					verdict = Verdict::Cycle;
					break;
				}
				if (steps >= cap) {
					verdict = Verdict::CapExceeded;
					break;
				}
				if (lambda == power) {
					tortoise = hare;
					power <<= 1;
					lambda = 0;
				}
				hare = nextOf(hare);
				++lambda;
				++steps;
			}
		}
		if (stepsOut)
			*stepsOut = steps;
		return verdict;
	}

	/**
	 * @brief Finds the node whose successor closes a cycle reachable from `head`.
	 *
	 * Severing that node's link leaves every node reachable from `head` visited exactly once, so it
	 * repairs the chain in place. Floyd's algorithm locates the cycle entry, then the node that
	 * points back at it. Every phase is bounded by `cap` successor reads.
	 * @return The closing node, or null if the chain is acyclic or no cycle was found within `cap`.
	 */
	template <typename Node, typename NextFn>
	const Node* FindCycleClosingNode(const Node* head, NextFn&& nextOf, uint32_t cap)
	{
		if (!head)
			return nullptr;

		const Node* slow = head;
		const Node* fast = head;
		uint32_t steps = 0;
		do {
			fast = nextOf(fast);
			if (!fast)
				return nullptr;
			fast = nextOf(fast);
			if (!fast)
				return nullptr;
			slow = nextOf(slow);
			if (++steps > cap)
				return nullptr;
		} while (slow != fast);

		slow = head;
		for (steps = 0; slow != fast; ++steps) {
			if (steps > cap)
				return nullptr;
			slow = nextOf(slow);
			fast = nextOf(fast);
		}

		const Node* closing = slow;
		for (steps = 0; nextOf(closing) != slow; ++steps) {
			if (steps > cap)
				return nullptr;
			closing = nextOf(closing);
		}
		return closing;
	}
}
