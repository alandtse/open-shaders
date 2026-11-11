// Unit tests for DLSSGInputFenceTracker, which decides when the D3D11 copy of the shared
// depth and motion vectors must wait for DLSS-G to finish reading the last present's inputs.

#include "Features/Upscaling/DLSSGInputFence.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
	struct FakeFence
	{};
}

TEST_CASE("A fresh tracker never waits", "[DLSSGInputFence]")
{
	DLSSGInputFenceTracker<FakeFence> tracker;
	CHECK(tracker.GetFence() == nullptr);
	CHECK_FALSE(tracker.IsBehind(0));
}

TEST_CASE("The copy waits only while the fence is short of the last present", "[DLSSGInputFence]")
{
	FakeFence fence;
	DLSSGInputFenceTracker<FakeFence> tracker;
	tracker.Track(&fence, 5);
	CHECK(tracker.GetFence() == &fence);
	CHECK(tracker.GetTarget() == 5);
	CHECK(tracker.IsBehind(4));
	CHECK_FALSE(tracker.IsBehind(5));
	CHECK_FALSE(tracker.IsBehind(6));
}

TEST_CASE("An older fence value does not lower the target", "[DLSSGInputFence]")
{
	FakeFence fence;
	DLSSGInputFenceTracker<FakeFence> tracker;
	tracker.Track(&fence, 8);
	tracker.Track(&fence, 7);
	CHECK(tracker.GetTarget() == 8);
}

TEST_CASE("A recreated fence discards the old target", "[DLSSGInputFence]")
{
	FakeFence oldFence, newFence;
	DLSSGInputFenceTracker<FakeFence> tracker;
	tracker.Track(&oldFence, 1000);
	tracker.Track(&newFence, 3);
	CHECK(tracker.GetFence() == &newFence);
	CHECK(tracker.GetTarget() == 3);
	CHECK_FALSE(tracker.IsBehind(3));
}

TEST_CASE("A null fence stops waiting", "[DLSSGInputFence]")
{
	FakeFence fence;
	DLSSGInputFenceTracker<FakeFence> tracker;
	tracker.Track(&fence, 5);
	tracker.Track(nullptr, 9);
	CHECK(tracker.GetFence() == nullptr);
	CHECK_FALSE(tracker.IsBehind(0));
}

TEST_CASE("A fence with no presented value yet does not wait", "[DLSSGInputFence]")
{
	FakeFence fence;
	DLSSGInputFenceTracker<FakeFence> tracker;
	tracker.Track(&fence, 0);
	CHECK(tracker.GetFence() == nullptr);
	CHECK_FALSE(tracker.IsBehind(0));
}

TEST_CASE("Reset forgets a fence whose resources were freed", "[DLSSGInputFence]")
{
	FakeFence fence;
	DLSSGInputFenceTracker<FakeFence> tracker;
	tracker.Track(&fence, 5);
	tracker.Reset();
	CHECK(tracker.GetFence() == nullptr);
	CHECK(tracker.GetTarget() == 0);
	CHECK_FALSE(tracker.IsBehind(0));
}
