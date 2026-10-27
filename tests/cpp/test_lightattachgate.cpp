// Unit tests for LightAttachGate::Tracker, which decides when an unchanged light's per-frame attach walk can be skipped.

#include "Features/LightLimitFix/LightAttachGate.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using LightAttachGate::Signature;
using LightAttachGate::Tracker;

namespace
{
	Signature SignatureOf(std::uint32_t a_value)
	{
		Signature signature{};
		signature[0] = a_value;
		return signature;
	}

	const void* LightAt(std::uintptr_t a_index)
	{
		return reinterpret_cast<const void*>(0x10000 + a_index * 0x140);
	}

	// Mirrors the engine call site: decide, walk, then record what the walk left behind.
	bool Visit(Tracker& a_tracker, const void* a_light, const Signature& a_signature, std::uint64_t a_frame, std::uint32_t a_refresh)
	{
		const bool walk = a_tracker.ShouldWalk(a_light, a_signature, a_frame, a_refresh);
		if (walk)
			a_tracker.RecordWalk(a_light, a_signature);
		return walk;
	}
}

TEST_CASE("A light seen for the first time is walked", "[LightAttachGate]")
{
	Tracker tracker;
	CHECK(Visit(tracker, LightAt(0), SignatureOf(1), 0, 60));
}

TEST_CASE("An unchanged light is skipped between its refreshes", "[LightAttachGate]")
{
	Tracker tracker;
	Visit(tracker, LightAt(0), SignatureOf(1), 0, 60);
	int walks = 0;
	for (std::uint64_t frame = 1; frame <= 600; ++frame)
		walks += Visit(tracker, LightAt(0), SignatureOf(1), frame, 60) ? 1 : 0;
	CHECK(walks == 10);
}

TEST_CASE("A changed signature is walked on the frame it changes", "[LightAttachGate]")
{
	Tracker tracker;
	Visit(tracker, LightAt(0), SignatureOf(1), 0, 1000);
	CHECK_FALSE(Visit(tracker, LightAt(0), SignatureOf(1), 1, 1000));
	CHECK(Visit(tracker, LightAt(0), SignatureOf(2), 2, 1000));
	CHECK_FALSE(Visit(tracker, LightAt(0), SignatureOf(2), 3, 1000));
}

TEST_CASE("A refresh interval of zero walks every light every frame", "[LightAttachGate]")
{
	Tracker tracker;
	for (std::uint64_t frame = 0; frame < 5; ++frame)
		CHECK(Visit(tracker, LightAt(0), SignatureOf(1), frame, 0));
}

TEST_CASE("Refreshes are staggered across lights", "[LightAttachGate]")
{
	constexpr std::uint32_t kRefresh = 10;
	constexpr std::uint32_t kLights = 100;
	Tracker tracker;
	for (std::uint32_t light = 0; light < kLights; ++light)
		Visit(tracker, LightAt(light), SignatureOf(1), 0, kRefresh);

	std::uint32_t busiest = 0;
	std::uint32_t total = 0;
	for (std::uint64_t frame = 1; frame <= kRefresh; ++frame) {
		std::uint32_t walks = 0;
		for (std::uint32_t light = 0; light < kLights; ++light)
			walks += Visit(tracker, LightAt(light), SignatureOf(1), frame, kRefresh) ? 1 : 0;
		busiest = std::max(busiest, walks);
		total += walks;
	}
	CHECK(total == kLights);
	CHECK(busiest == kLights / kRefresh);
}

TEST_CASE("Lights are tracked independently", "[LightAttachGate]")
{
	Tracker tracker;
	Visit(tracker, LightAt(0), SignatureOf(1), 0, 1000);
	Visit(tracker, LightAt(1), SignatureOf(1), 0, 1000);
	CHECK(Visit(tracker, LightAt(0), SignatureOf(2), 1, 1000));
	CHECK_FALSE(Visit(tracker, LightAt(1), SignatureOf(1), 1, 1000));
}

TEST_CASE("A walk that was never recorded is repeated", "[LightAttachGate]")
{
	Tracker tracker;
	CHECK(tracker.ShouldWalk(LightAt(0), SignatureOf(1), 0, 1000));
	CHECK(tracker.ShouldWalk(LightAt(0), SignatureOf(1), 1, 1000));
}

TEST_CASE("Filling the table empties it so every light is walked once more", "[LightAttachGate]")
{
	Tracker tracker;
	for (std::size_t light = 0; light < Tracker::kMaxTrackedLights; ++light)
		Visit(tracker, LightAt(light), SignatureOf(1), 0, 1000);
	CHECK(tracker.Size() == Tracker::kMaxTrackedLights);
	CHECK(Visit(tracker, LightAt(Tracker::kMaxTrackedLights), SignatureOf(1), 1, 1000));
	CHECK(tracker.Size() == 1);
	CHECK(Visit(tracker, LightAt(0), SignatureOf(1), 2, 1000));
}

TEST_CASE("Clear forgets every light", "[LightAttachGate]")
{
	Tracker tracker;
	Visit(tracker, LightAt(0), SignatureOf(1), 0, 1000);
	tracker.Clear();
	CHECK(tracker.Size() == 0);
	CHECK(Visit(tracker, LightAt(0), SignatureOf(1), 1, 1000));
}

TEST_CASE("A forgotten light is walked on its next visit", "[LightAttachGate]")
{
	Tracker tracker;
	Visit(tracker, LightAt(0), SignatureOf(1), 0, 1000);
	Visit(tracker, LightAt(1), SignatureOf(1), 0, 1000);
	tracker.Forget(LightAt(0));
	CHECK(Visit(tracker, LightAt(0), SignatureOf(1), 1, 1000));
	CHECK_FALSE(Visit(tracker, LightAt(1), SignatureOf(1), 1, 1000));
	tracker.Forget(LightAt(7));
	CHECK(tracker.Size() == 2);
}
