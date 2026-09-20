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
		bool capped = false;
		const auto read = [&](const Node* node) -> const Node* {
			if (steps >= cap) {
				capped = true;
				return nullptr;
			}
			++steps;
			return nextOf(node);
		};
		Verdict verdict = Verdict::Clean;
		if (head) {
			const Node* tortoise = head;
			const Node* hare = read(head);
			uint32_t power = 1;
			uint32_t lambda = 1;
			while (hare) {
				if (hare == tortoise) {
					verdict = Verdict::Cycle;
					break;
				}
				if (lambda == power) {
					tortoise = hare;
					power <<= 1;
					lambda = 0;
				}
				hare = read(hare);
				++lambda;
			}
			if (capped)
				verdict = Verdict::CapExceeded;
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

		uint32_t reads = 0;
		const auto read = [&](const Node* node) -> const Node* {
			if (reads >= cap)
				return nullptr;
			++reads;
			return nextOf(node);
		};

		const Node* slow = head;
		const Node* fast = head;
		do {
			fast = read(fast);
			fast = fast ? read(fast) : nullptr;
			slow = read(slow);
			if (!fast || !slow)
				return nullptr;
		} while (slow != fast);

		slow = head;
		while (slow != fast) {
			slow = read(slow);
			fast = read(fast);
			if (!slow || !fast)
				return nullptr;
		}

		const Node* closing = slow;
		for (;;) {
			const Node* next = read(closing);
			if (!next)
				return nullptr;
			if (next == slow)
				return closing;
			closing = next;
		}
	}
}
