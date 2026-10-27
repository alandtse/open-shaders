// Unit tests for Util::ClassifyOwnerAncestry, the parent-chain walk that decides whether a shadow
// caster is attached to an actor.

#include "Utils/OwnerAncestry.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
	struct FakeNode
	{
		Util::OwnerKind owner = Util::OwnerKind::None;
		const FakeNode* parent = nullptr;
	};

	Util::OwnerAncestry Classify(const FakeNode& a_node)
	{
		return Util::ClassifyOwnerAncestry(&a_node, [](const FakeNode& a_n) { return a_n.owner; });
	}
}

TEST_CASE("A chain with no owner classifies as None", "[ownerancestry]")
{
	const FakeNode root;
	const FakeNode mid{ Util::OwnerKind::None, &root };
	const FakeNode leaf{ Util::OwnerKind::None, &mid };
	REQUIRE(Classify(leaf) == Util::OwnerAncestry::None);
	REQUIRE_FALSE(Util::IsActorAttached(Classify(leaf)));
}

TEST_CASE("A node owned by an actor classifies as Actor", "[ownerancestry]")
{
	const FakeNode leaf{ Util::OwnerKind::Actor, nullptr };
	REQUIRE(Classify(leaf) == Util::OwnerAncestry::Actor);
	REQUIRE(Util::IsActorAttached(Classify(leaf)));
}

TEST_CASE("A weapon mesh under an actor skeleton classifies as ActorAncestor", "[ownerancestry]")
{
	const FakeNode actorRoot{ Util::OwnerKind::Actor, nullptr };
	const FakeNode handBone{ Util::OwnerKind::None, &actorRoot };
	const FakeNode weapon{ Util::OwnerKind::None, &handBone };
	REQUIRE(Classify(weapon) == Util::OwnerAncestry::ActorAncestor);
	REQUIRE(Util::IsActorAttached(Classify(weapon)));
}

TEST_CASE("The nearest owner decides, so a prop under a non-actor reference stays a Reference", "[ownerancestry]")
{
	const FakeNode actorRoot{ Util::OwnerKind::Actor, nullptr };
	const FakeNode prop{ Util::OwnerKind::Reference, &actorRoot };
	const FakeNode mesh{ Util::OwnerKind::None, &prop };
	REQUIRE(Classify(mesh) == Util::OwnerAncestry::Reference);
	REQUIRE_FALSE(Util::IsActorAttached(Classify(mesh)));
}

TEST_CASE("An owner deeper than the depth cap is not found", "[ownerancestry]")
{
	const FakeNode actorRoot{ Util::OwnerKind::Actor, nullptr };
	FakeNode chain[Util::MaxOwnerAncestorDepth + 1];
	chain[0].parent = &actorRoot;
	for (int i = 1; i <= Util::MaxOwnerAncestorDepth; ++i)
		chain[i].parent = &chain[i - 1];
	REQUIRE(Classify(chain[Util::MaxOwnerAncestorDepth]) == Util::OwnerAncestry::None);
	REQUIRE(Classify(chain[Util::MaxOwnerAncestorDepth - 2]) == Util::OwnerAncestry::ActorAncestor);
}
