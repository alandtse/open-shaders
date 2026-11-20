// Unit tests for ShadowCasterManager pure helpers
// (src/Features/LightLimitFix/ShadowCasterMath.h): the shadow-light pointer
// plausibility check and the frame-time 90th-percentile.

#include "Features/LightLimitFix/ShadowCasterMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using Catch::Approx;
using ShadowCasterManager::DemandStreakBucket;
using ShadowCasterManager::EngineShadowSliceCount;
using ShadowCasterManager::FacingRotation;
using ShadowCasterManager::FlickerCoreRadius;
using ShadowCasterManager::FlickerPositionToleranceSq;
using ShadowCasterManager::FlickerRadiusBand;
using ShadowCasterManager::FlickerRadiusTolerance;
using ShadowCasterManager::FrameTimePercentile;
using ShadowCasterManager::HashCombine;
using ShadowCasterManager::HashCombineFloat;
using ShadowCasterManager::HeldFlickerRadius;
using ShadowCasterManager::HeldLightYields;
using ShadowCasterManager::HemisphereSeamMargin;
using ShadowCasterManager::HoldsFlame;
using ShadowCasterManager::InFlickerCore;
using ShadowCasterManager::IsPlausibleShadowLightPtr;
using ShadowCasterManager::kFlickerReachPerAmplitude;
using ShadowCasterManager::kMaxFlickerRadiusTolerance;
using ShadowCasterManager::kShadowSizeToTexels;
using ShadowCasterManager::kSlotHoldMargin;
using ShadowCasterManager::kTileScaleFloor;
using ShadowCasterManager::kVanillaShadowSliceCount;
using ShadowCasterManager::PlayerProtectionCap;
using ShadowCasterManager::ProjectedLightExtent;
using ShadowCasterManager::QuantizeFloat;
using ShadowCasterManager::SliceCappedLightCount;
using ShadowCasterManager::SlotHeldScore;
using ShadowCasterManager::SphereWhollyBehindPlane;
using ShadowCasterManager::StallBucket;
using ShadowCasterManager::SteadyScoreFade;
using ShadowCasterManager::TileScaleForCoverage;
using ShadowCasterManager::TileScaleTarget;
using ShadowCasterManager::WithinFlickerReach;

namespace
{
	// sizeProxy that asks for exactly `texels` at the classifier's scale.
	float SizeForTexels(float texels)
	{
		return texels / kShadowSizeToTexels;
	}
}

TEST_CASE("TileScaleTarget picks the smallest class meeting the target", "[scm]")
{
	// 2048 base: targets above half-class resolution stay full.
	REQUIRE(TileScaleTarget(SizeForTexels(2000.0f), 2048.0f) == 1.0f);
	REQUIRE(TileScaleTarget(SizeForTexels(1025.0f), 2048.0f) == 1.0f);
	// Exactly satisfiable by a smaller class steps down.
	REQUIRE(TileScaleTarget(SizeForTexels(1024.0f), 2048.0f) == 0.5f);
	REQUIRE(TileScaleTarget(SizeForTexels(512.0f), 2048.0f) == 0.25f);
	// Tiny lights clamp at the ladder floor, never zero.
	REQUIRE(TileScaleTarget(0.0f, 2048.0f) == kTileScaleFloor);
	// The base resolution scales the whole ladder.
	REQUIRE(TileScaleTarget(SizeForTexels(1024.0f), 4096.0f) == 0.25f);
}

TEST_CASE("TileScaleForCoverage promotes immediately", "[scm]")
{
	REQUIRE(TileScaleForCoverage(SizeForTexels(2000.0f), 2048.0f, 0.25f) == 1.0f);
	REQUIRE(TileScaleForCoverage(SizeForTexels(700.0f), 2048.0f, 0.25f) == 0.5f);
}

TEST_CASE("TileScaleForCoverage demotes lazily (hysteresis)", "[scm]")
{
	// Just below the class boundary: the headroom-inflated size still asks
	// for the current class, so it holds (a flip would force a redraw).
	REQUIRE(TileScaleForCoverage(SizeForTexels(900.0f), 2048.0f, 1.0f) == 1.0f);
	// Clearly below even with headroom: demotes to the target class.
	REQUIRE(TileScaleForCoverage(SizeForTexels(600.0f), 2048.0f, 1.0f) == 0.5f);
	REQUIRE(TileScaleForCoverage(SizeForTexels(300.0f), 2048.0f, 0.5f) == 0.25f);
	// The extended ladder reaches the sixteenth class for tiny lights.
	REQUIRE(TileScaleForCoverage(SizeForTexels(100.0f), 2048.0f, 0.5f) == kTileScaleFloor);
}

TEST_CASE("IsPlausibleShadowLightPtr rejects null, near-null, misaligned, and non-canonical", "[scm]")
{
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0));                      // null
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x8));                    // near-null: below the 64 KiB floor
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0xFFF8ull));              // aligned but still below the floor
	REQUIRE(IsPlausibleShadowLightPtr(0x10000ull));                   // at the floor, aligned -> plausible
	REQUIRE(IsPlausibleShadowLightPtr(0x00007FFFFFFFFFF8ull));        // top of user-mode range
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x0000800000000000ull));  // first non-canonical
	REQUIRE_FALSE(IsPlausibleShadowLightPtr(0xFFFFF80000000000ull));  // kernel-space garbage

	// Any non-8-byte alignment is rejected (use a base above the floor so the
	// alignment check is what fails, not the minimum-address check).
	for (std::uintptr_t off = 1; off < 8; ++off)
		REQUIRE_FALSE(IsPlausibleShadowLightPtr(0x10000 + off));
}

TEST_CASE("HemisphereSeamMargin floors a small light radius", "[scm]")
{
	// 0.5% of 1024 = 5.12, above the floor.
	REQUIRE(HemisphereSeamMargin(1024.0f) == Approx(5.12f));
	// Degenerate and small radii clamp to the floor instead of collapsing to 0.
	REQUIRE(HemisphereSeamMargin(0.0f) == Approx(1.0f));
	REQUIRE(HemisphereSeamMargin(100.0f) == Approx(1.0f));
}

TEST_CASE("FacingRotation aims its X column and stays a rotation", "[scm]")
{
	float r[3][3]{};
	REQUIRE(FacingRotation(30.0f, -40.0f, 0.0f, r));
	REQUIRE(r[0][0] == Approx(0.6f));
	REQUIRE(r[1][0] == Approx(-0.8f));
	REQUIRE(r[2][0] == Approx(0.0f).margin(1e-6));
	REQUIRE(r[2][1] == Approx(1.0f));
	const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
	                  r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
	REQUIRE(det == Approx(1.0f));
	// Straight up has no upright: it still makes a rotation.
	REQUIRE(FacingRotation(0.0f, 0.0f, 10.0f, r));
	REQUIRE(r[2][0] == Approx(1.0f));
	REQUIRE(FacingRotation(0.0f, 0.0f, -10.0f, r));
	REQUIRE(r[2][0] == Approx(-1.0f));
	REQUIRE_FALSE(FacingRotation(0.1f, 0.0f, 0.0f, r));
}

TEST_CASE("SphereWhollyBehindPlane splits straddling from wholly behind", "[scm]")
{
	const float margin = HemisphereSeamMargin(1024.0f);  // 5.12

	// In front of the plane, or straddling it by any amount: the front map can
	// still see part of the sphere, so the engine's hemisphere choice stands.
	REQUIRE_FALSE(SphereWhollyBehindPlane(100.0f, 10.0f, margin));
	REQUIRE_FALSE(SphereWhollyBehindPlane(0.0f, 10.0f, margin));
	REQUIRE_FALSE(SphereWhollyBehindPlane(-10.0f, 10.0f, margin));
	// Entirely behind, but within the seam margin: also left alone.
	REQUIRE_FALSE(SphereWhollyBehindPlane(-15.0f, 10.0f, margin));
	REQUIRE_FALSE(SphereWhollyBehindPlane(-10.0f - margin + 0.01f, 10.0f, margin));
	// Behind by more than the margin: nothing of it reaches the front map.
	REQUIRE(SphereWhollyBehindPlane(-10.0f - margin - 0.01f, 10.0f, margin));
	REQUIRE(SphereWhollyBehindPlane(-500.0f, 10.0f, margin));
}

TEST_CASE("WithinFlickerReach covers every flicker and pulse offset", "[scm]")
{
	constexpr float amplitude = 6.0f;
	for (int i = 0; i < 64; ++i) {
		const float a = static_cast<float>(i) * 0.1f;
		// Pulse: a circle of the amplitude, their product on z.
		REQUIRE(WithinFlickerReach(amplitude * std::cos(a), amplitude * std::sin(a), amplitude * std::sin(a) * std::cos(a), amplitude));
		for (int j = 0; j < 64; ++j) {
			const float b = static_cast<float>(j) * 0.1f;
			// Flicker: two sines scale half the amplitude, which then moves along them and a third sine.
			const float scaled = amplitude * std::sin(a) * std::sin(b) * 0.5f;
			REQUIRE(WithinFlickerReach(scaled * std::sin(a), scaled * std::sin(b), scaled, amplitude));
		}
	}
	REQUIRE(WithinFlickerReach(0.0f, 0.0f, 0.0f, amplitude));
	// No movement amplitude, or a translate the movement cannot reach.
	REQUIRE_FALSE(WithinFlickerReach(0.0f, 0.0f, 0.0f, 0.0f));
	REQUIRE_FALSE(WithinFlickerReach(0.0f, 0.0f, 8.0f, amplitude));
	REQUIRE_FALSE(WithinFlickerReach(40.0f, 0.0f, 0.0f, amplitude));
}

TEST_CASE("HeldFlickerRadius holds through every flickered radius and follows a real change", "[scm]")
{
	constexpr float brightest = 520.0f;
	constexpr float amplitude = 0.3f;
	const float band = FlickerRadiusBand(amplitude);
	// The fade scales between 1 - amplitude and 1; the radius follows its square root.
	for (int start = 0; start <= 10; ++start) {
		const float held = brightest * std::sqrt(1.0f - amplitude * static_cast<float>(start) / 10.0f);
		for (int i = 0; i <= 100; ++i)
			REQUIRE(HeldFlickerRadius(held, brightest * std::sqrt(1.0f - amplitude * static_cast<float>(i) / 100.0f), band) == held);
	}
	REQUIRE(HeldFlickerRadius(brightest, brightest * 1.5f, band) == brightest * 1.5f);
	REQUIRE(HeldFlickerRadius(brightest, brightest * 0.5f, band) == brightest * 0.5f);
	// No flicker: only a sub-unit change is held.
	REQUIRE(FlickerRadiusBand(0.0f) == 0.0f);
	REQUIRE(HeldFlickerRadius(brightest, brightest + 0.5f, 0.0f) == brightest);
	REQUIRE(HeldFlickerRadius(brightest, brightest + 2.0f, 0.0f) == brightest + 2.0f);
}

TEST_CASE("FlickerCoreRadius stays inside every radius and offset the flicker reaches", "[scm]")
{
	constexpr float brightest = 520.0f;
	constexpr float amplitude = 0.3f;
	constexpr float movement = 6.0f;
	const float band = FlickerRadiusBand(amplitude);
	for (int start = 0; start <= 10; ++start) {
		const float held = brightest * std::sqrt(1.0f - amplitude * static_cast<float>(start) / 10.0f);
		const float core = FlickerCoreRadius(held, band, movement);
		for (int i = 0; i <= 100; ++i) {
			const float live = brightest * std::sqrt(1.0f - amplitude * static_cast<float>(i) / 100.0f);
			REQUIRE(HeldFlickerRadius(held, live, band) == held);
			REQUIRE(core + movement * kFlickerReachPerAmplitude <= live);
		}
	}
	REQUIRE(FlickerCoreRadius(brightest, 0.0f, 0.0f) == brightest - 1.0f);
	REQUIRE(FlickerCoreRadius(5.0f, 0.9f, movement) == 0.0f);
}

TEST_CASE("FlickerRadiusTolerance covers a bake and a live radius anywhere in the band", "[scm]")
{
	constexpr float brightest = 520.0f;
	constexpr float amplitude = 0.3f;
	const float tolerance = FlickerRadiusTolerance(FlickerRadiusBand(amplitude), 0.1f);
	for (int b = 0; b <= 20; ++b) {
		const float bake = brightest * std::sqrt(1.0f - amplitude * static_cast<float>(b) / 20.0f);
		for (int i = 0; i <= 20; ++i) {
			const float live = brightest * std::sqrt(1.0f - amplitude * static_cast<float>(i) / 20.0f);
			REQUIRE(std::abs(live - bake) <= tolerance * bake + 1e-3f);
		}
	}
	REQUIRE(FlickerRadiusTolerance(0.0f, 0.1f) == 0.1f);
	REQUIRE(FlickerRadiusTolerance(FlickerRadiusBand(0.9f), 0.1f) == kMaxFlickerRadiusTolerance);
}

TEST_CASE("FlickerPositionToleranceSq covers a bake and a live position anywhere in the flicker", "[scm]")
{
	constexpr float movement = 3.93f;
	constexpr float floorSq = 16.0f;
	const float reach = movement * kFlickerReachPerAmplitude;
	REQUIRE(WithinFlickerReach(reach, 0.0f, 0.0f, movement));
	REQUIRE(WithinFlickerReach(-reach, 0.0f, 0.0f, movement));
	REQUIRE(4.0f * reach * reach <= FlickerPositionToleranceSq(movement, floorSq));
	REQUIRE(FlickerPositionToleranceSq(0.0f, floorSq) == floorSq);
	REQUIRE(FlickerPositionToleranceSq(0.5f, floorSq) == floorSq);
}

TEST_CASE("InFlickerCore hashes only the static casters a flickering light always reaches", "[scm]")
{
	// Radius 520 at intensity amplitude 0.3 with no movement: a core of about 434 units.
	const float core = FlickerCoreRadius(520.0f, FlickerRadiusBand(0.3f), 0.0f);
	REQUIRE(core == Approx(434.06f).margin(0.05f));
	REQUIRE_FALSE(InFlickerCore(480.0f, 0.0f, 0.0f, 40.0f, core));
	REQUIRE(InFlickerCore(480.0f, 0.0f, 0.0f, 50.0f, core));
	REQUIRE(InFlickerCore(0.0f, 300.0f, 0.0f, 1.0f, core));
	REQUIRE(InFlickerCore(10.0f, 0.0f, 0.0f, 1.0f, 9.5f));
	REQUIRE_FALSE(InFlickerCore(10.0f, 0.0f, 0.0f, 0.4f, 9.5f));
	REQUIRE(InFlickerCore(0.0f, 0.0f, 0.0f, 0.0f, 1.0f));
	REQUIRE(InFlickerCore(1.0e6f, 0.0f, 0.0f, 1.0f, std::numeric_limits<float>::infinity()));
}

TEST_CASE("FrameTimePercentile returns the 60fps fallback with no samples", "[scm]")
{
	float ring[8]{};
	REQUIRE(FrameTimePercentile(ring, 0) == Approx(16.67f));
	// A negative count (corruption / future refactor) must not drive a negative
	// n into std::copy / std::nth_element -- it takes the fallback too.
	REQUIRE(FrameTimePercentile(ring, -1) == Approx(16.67f));
}

TEST_CASE("FrameTimePercentile picks the P90 element by default", "[scm]")
{
	// 10 samples 1..10: idx = int(10*0.9) = 9 -> the largest sorted element.
	float ring[10] = { 5, 2, 9, 1, 7, 3, 10, 4, 8, 6 };
	REQUIRE(FrameTimePercentile(ring, 10) == Approx(10.0f));
}

TEST_CASE("FrameTimePercentile honors count below the window size", "[scm]")
{
	// Only the first 5 entries are valid; trailing slots must not be sampled.
	float ring[10] = { 10, 20, 30, 40, 50, 999, 999, 999, 999, 999 };
	// n = 5, idx = int(5*0.9) = 4 -> the largest of {10..50} = 50.
	REQUIRE(FrameTimePercentile(ring, 5) == Approx(50.0f));
}

TEST_CASE("FrameTimePercentile clamps count above the window size", "[scm]")
{
	// count > Window: std::min clamps n to Window (10) so the copy stays in
	// bounds; all slots are sampled. Pins the clamp against a regression.
	float ring[10] = { 5, 2, 9, 1, 7, 3, 10, 4, 8, 6 };
	REQUIRE(FrameTimePercentile(ring, 99) == Approx(10.0f));
}

TEST_CASE("FrameTimePercentile honors an explicit percentile", "[scm]")
{
	float ring[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	REQUIRE(FrameTimePercentile(ring, 10, 0.5f) == Approx(6.0f));
	REQUIRE(FrameTimePercentile(ring, 10, 0.1f) == Approx(2.0f));
}

TEST_CASE("HashCombine is order-sensitive and deterministic", "[scm]")
{
	const uint64_t seed = 0x9e3779b97f4a7c15ull;
	REQUIRE(HashCombine(seed, 1u) == HashCombine(seed, 1u));
	REQUIRE(HashCombine(seed, 1u) != HashCombine(seed, 2u));
	// Folding the same two values in a different order must not collide --
	// this is what lets ComputeShadowGeomHash distinguish caster sets.
	const uint64_t ab = HashCombine(HashCombine(seed, 1u), 2u);
	const uint64_t ba = HashCombine(HashCombine(seed, 2u), 1u);
	REQUIRE(ab != ba);
}

TEST_CASE("HashCombineFloat distinguishes bit-distinct floats", "[scm]")
{
	const uint64_t seed = 0x9e3779b97f4a7c15ull;
	REQUIRE(HashCombineFloat(seed, 1.0f) == HashCombineFloat(seed, 1.0f));
	REQUIRE(HashCombineFloat(seed, 1.0f) != HashCombineFloat(seed, 1.0001f));
	REQUIRE(HashCombineFloat(seed, 0.0f) != HashCombineFloat(seed, -0.0f));
}

TEST_CASE("QuantizeFloat snaps to the nearest step multiple", "[scm]")
{
	REQUIRE(QuantizeFloat(0.4f, 1.0f) == Approx(0.0f));
	REQUIRE(QuantizeFloat(0.6f, 1.0f) == Approx(1.0f));
	REQUIRE(QuantizeFloat(12.3f, 4.0f) == Approx(12.0f));
	// Sub-step motion collapses to the same bucket -- the whole point of
	// quantizing before hashing (flicker/jitter shouldn't bust the cache).
	REQUIRE(QuantizeFloat(10.01f, 1.0f) == QuantizeFloat(10.04f, 1.0f));
}

TEST_CASE("StallBucket boundaries match the histogram ladder", "[scm]")
{
	REQUIRE(StallBucket(0) == 0);
	REQUIRE(StallBucket(1) == 1);
	REQUIRE(StallBucket(2) == 1);
	REQUIRE(StallBucket(3) == 2);
	REQUIRE(StallBucket(7) == 2);
	REQUIRE(StallBucket(8) == 3);
	REQUIRE(StallBucket(15) == 3);
	REQUIRE(StallBucket(16) == 4);
	REQUIRE(StallBucket(44) == 4);
	REQUIRE(StallBucket(45) == 5);
	REQUIRE(StallBucket(1000) == 5);
}

TEST_CASE("DemandStreakBucket boundaries match the histogram ladder", "[scm]")
{
	REQUIRE(DemandStreakBucket(0) == 0);
	REQUIRE(DemandStreakBucket(2) == 1);
	REQUIRE(DemandStreakBucket(3) == 2);
	REQUIRE(DemandStreakBucket(7) == 2);
	REQUIRE(DemandStreakBucket(8) == 3);
	REQUIRE(DemandStreakBucket(15) == 3);
	REQUIRE(DemandStreakBucket(16) == 4);
	REQUIRE(DemandStreakBucket(31) == 4);
	REQUIRE(DemandStreakBucket(32) == 5);
	REQUIRE(DemandStreakBucket(63) == 5);
	REQUIRE(DemandStreakBucket(64) == 6);
	REQUIRE(DemandStreakBucket(127) == 6);
	REQUIRE(DemandStreakBucket(128) == 7);
	REQUIRE(DemandStreakBucket(255) == 7);
	REQUIRE(DemandStreakBucket(256) == 8);
	REQUIRE(DemandStreakBucket(100000) == 8);
}

TEST_CASE("EngineShadowSliceCount leaves a slice for the sun's pool index", "[scm]")
{
	REQUIRE(EngineShadowSliceCount(0) == kVanillaShadowSliceCount);
	REQUIRE(EngineShadowSliceCount(4) == kVanillaShadowSliceCount);
	REQUIRE(EngineShadowSliceCount(7) == kVanillaShadowSliceCount);
	REQUIRE(EngineShadowSliceCount(8) == 9);
	REQUIRE(EngineShadowSliceCount(16) == 17);
	REQUIRE(EngineShadowSliceCount(127) == 128);
}

TEST_CASE("SliceCappedLightCount keeps the highest point index inside the array", "[scm]")
{
	REQUIRE(SliceCappedLightCount(8, kVanillaShadowSliceCount) == 7);
	REQUIRE(SliceCappedLightCount(16, kVanillaShadowSliceCount) == 7);
	REQUIRE(SliceCappedLightCount(5, kVanillaShadowSliceCount) == 5);
	REQUIRE(SliceCappedLightCount(8, 0) == 0);
	// Point lights index slices 1..count beside the sun, so count must stay below slices.
	for (int32_t slices = 1; slices <= 129; ++slices)
		for (int32_t count = 0; count <= 127; ++count)
			REQUIRE(SliceCappedLightCount(count, slices) < slices);
}

TEST_CASE("Shadow ranking clips lights crossing the camera plane", "[scm]")
{
	REQUIRE(ProjectedLightExtent(0.0f, 1.0f, -3.0f, -1.0f) == 0.0f);
	REQUIRE(ProjectedLightExtent(1.7f, 0.3f, -0.2f, 0.7f) == 0.0f);
	REQUIRE(ProjectedLightExtent(0.9f, 0.3f, -0.1f, 1.0f) == Approx(0.2f));
	REQUIRE(ProjectedLightExtent(-0.9f, 0.3f, -0.1f, 1.0f) == Approx(0.2f));
	REQUIRE(ProjectedLightExtent(0.0f, 0.5f, -0.1f, 1.0f) == 1.0f);
}

TEST_CASE("HeldLightYields seats only lights out of view after the lights in view", "[scm]")
{
	REQUIRE(HeldLightYields(false, false, false));
	// Still in view (an engine cull flap at the edge), reaching the player, or hidden: keeps its place.
	REQUIRE_FALSE(HeldLightYields(true, false, false));
	REQUIRE_FALSE(HeldLightYields(false, true, false));
	REQUIRE_FALSE(HeldLightYields(false, false, true));
}

TEST_CASE("SlotHeldScore keeps a holder against a near-equal challenger", "[scm]")
{
	REQUIRE(SlotHeldScore(0.5, false) == 0.5);
	REQUIRE(SlotHeldScore(0.5, true) == Approx(0.5 * kSlotHoldMargin));
	// A challenger inside the margin stays below the holder; one past it wins at once.
	REQUIRE(SlotHeldScore(0.5, true) > SlotHeldScore(0.5 * (kSlotHoldMargin - 0.01), false));
	REQUIRE(SlotHeldScore(0.5, true) < SlotHeldScore(0.5 * (kSlotHoldMargin + 0.01), false));
	// A user formula can score at or below zero, where scaling would demote the holder.
	REQUIRE(SlotHeldScore(0.0, true) == 0.0);
	REQUIRE(SlotHeldScore(-2.0, true) == -2.0);
}

TEST_CASE("Every light count fits the slices allocated for it", "[scm]")
{
	for (int32_t count = 0; count <= 127; ++count)
		REQUIRE(SliceCappedLightCount(count, EngineShadowSliceCount(count)) == count);
}

TEST_CASE("PerFrameCache works a key out once per frame and drops it on the next", "[scm]")
{
	ShadowCasterManager::PerFrameCache<int, int> cache;
	int made = 0;
	const auto make = [&made] { return ++made; };
	REQUIRE(cache.Get(10, 1, make) == 1);
	REQUIRE(cache.Get(10, 1, make) == 1);
	REQUIRE(cache.Get(10, 2, make) == 2);
	REQUIRE(made == 2);
	REQUIRE(cache.Get(11, 1, make) == 3);
	REQUIRE(cache.entries.size() == 1);
}

TEST_CASE("HoldsFlame takes only a small part round or under the flame", "[scm]")
{
	SECTION("a part round the flame within its bound and margin")
	{
		REQUIRE(HoldsFlame(0.0f, 0.0f, 2.0f, 3.0f, 1.0f));
		REQUIRE(HoldsFlame(0.0f, 0.0f, 7.0f, 3.0f, 1.0f));
		REQUIRE_FALSE(HoldsFlame(0.0f, 0.0f, 7.5f, 3.0f, 1.0f));
		REQUIRE(HoldsFlame(0.0f, 0.0f, 0.0f, 20.0f, 1.0f));
		REQUIRE_FALSE(HoldsFlame(0.0f, 0.0f, 0.0f, 20.5f, 1.0f));
		REQUIRE_FALSE(HoldsFlame(0.0f, 0.0f, 0.0f, 0.0f, 1.0f));
	}
	SECTION("a Grand soul gem's crystal, not a whole hanging lantern")
	{
		REQUIRE(HoldsFlame(0.0f, 0.0f, 12.0f, 17.0f, 1.0f));
		REQUIRE_FALSE(HoldsFlame(0.0f, 0.0f, 0.0f, 24.0f, 1.0f));
	}
	SECTION("a fixture placed at twice its size")
	{
		REQUIRE(HoldsFlame(0.0f, 0.0f, 0.0f, 40.0f, 2.0f));
		REQUIRE(HoldsFlame(0.0f, 0.0f, 14.0f, 6.0f, 2.0f));
		REQUIRE_FALSE(HoldsFlame(0.0f, 0.0f, 0.0f, 40.5f, 2.0f));
		REQUIRE(HoldsFlame(0.0f, 0.0f, 0.0f, 20.0f, 0.0f));
	}
	SECTION("a candle under a light hung over its wick, not one too far under, beside or over it")
	{
		REQUIRE(HoldsFlame(0.0f, 0.0f, -31.0f, 11.8f, 1.0f));
		REQUIRE_FALSE(HoldsFlame(0.0f, 0.0f, -40.0f, 11.8f, 1.0f));
		REQUIRE_FALSE(HoldsFlame(20.0f, 0.0f, -31.0f, 11.8f, 1.0f));
		REQUIRE_FALSE(HoldsFlame(0.0f, 0.0f, 31.0f, 11.8f, 1.0f));
		REQUIRE(HoldsFlame(0.0f, 0.0f, -62.0f, 23.6f, 2.0f));
	}
}

TEST_CASE("SteadyScoreFade averages a flicker but follows a light going out at once", "[scm]")
{
	float fade = 1.0f;
	float lowest = 2.0f, highest = 0.0f;
	for (int frame = 0; frame < 200; ++frame) {
		fade = SteadyScoreFade(fade, frame % 2 == 0 ? 0.6f : 1.0f);
		if (frame > 100) {
			lowest = std::min(lowest, fade);
			highest = std::max(highest, fade);
		}
	}
	REQUIRE(highest - lowest < 0.05f);
	REQUIRE(fade == Approx(0.8f).margin(0.03f));
	REQUIRE(SteadyScoreFade(0.8f, 0.0f) == 0.0f);
	REQUIRE(SteadyScoreFade(0.8f, 0.05f) == 0.05f);
	// A light coming back on is ranked at its live fade at once, not ramped up from its average.
	REQUIRE(SteadyScoreFade(0.0f, 1.0f) == 1.0f);
	REQUIRE(SteadyScoreFade(0.05f, 0.7f) == 0.7f);
}

TEST_CASE("PlayerProtectionCap leaves half the slots to the lights in view", "[scm]")
{
	REQUIRE(PlayerProtectionCap(16) == 8);
	REQUIRE(PlayerProtectionCap(127) == 63);
	REQUIRE(PlayerProtectionCap(1) == 1);
	REQUIRE(PlayerProtectionCap(0) == 1);
}
