#pragma once

#include <cstdint>

namespace Util
{
	/** @brief What owns a scene node: nothing, an actor, or any other reference. */
	enum class OwnerKind : std::uint8_t
	{
		None,
		Actor,
		Reference
	};

	/** @brief How a node relates to the first owning reference found walking toward the scene root. */
	enum class OwnerAncestry : std::uint8_t
	{
		None,
		Reference,
		Actor,
		ActorAncestor
	};

	inline constexpr int MaxOwnerAncestorDepth = 32;

	/**
	 * @brief Walks from a_node toward the root and classifies the first node that carries an owner.
	 * @param a_node Node to start from; its `parent` member links toward the root.
	 * @param a_kindOf Callable returning the OwnerKind of a node.
	 * @return Actor when a_node itself is owned by an actor, ActorAncestor when an ancestor is.
	 */
	template <class Node, class KindOf>
	[[nodiscard]] OwnerAncestry ClassifyOwnerAncestry(const Node* a_node, KindOf&& a_kindOf)
	{
		for (int depth = 0; a_node && depth < MaxOwnerAncestorDepth; ++depth, a_node = a_node->parent) {
			switch (a_kindOf(*a_node)) {
			case OwnerKind::Actor:
				return depth == 0 ? OwnerAncestry::Actor : OwnerAncestry::ActorAncestor;
			case OwnerKind::Reference:
				return OwnerAncestry::Reference;
			case OwnerKind::None:
				break;
			}
		}
		return OwnerAncestry::None;
	}

	/** @brief True when the node is owned by, or hangs under, an actor. */
	[[nodiscard]] constexpr bool IsActorAttached(OwnerAncestry a_ancestry) noexcept
	{
		return a_ancestry == OwnerAncestry::Actor || a_ancestry == OwnerAncestry::ActorAncestor;
	}
}
