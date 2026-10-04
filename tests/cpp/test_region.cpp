// Unit tests for the shared region helper: world-to-screen projection, the padded pixel crop,
// clamping and union of crops, the history-invalidation comparison, the reset policy and the crop
// stabiliser. Pure maths only; the source that feeds a crop needs a live game and is exercised there.

#include "Utils/Region.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

using Util::Region::ProjectionResult;
using Util::Region::RegionStabilizer;
using Util::Region::ResetPolicy;
using Util::Region::ScreenBounds;
using Util::Region::StereoRegion;
using Util::Subrect::PixelRegion;
using Util::Subrect::UVRegion;

namespace
{
	constexpr uint32_t kWidth = 1920;
	constexpr uint32_t kHeight = 1080;
	constexpr uint32_t kAlignmentPixels = Util::Region::kDefaultPixelAlignment;
	constexpr float kTolerancePixels = static_cast<float>(kAlignmentPixels);
	constexpr Util::Region::Padding kPadding{ 0.125f, 16.0f, 32.0f, 96.0f };
	constexpr Util::Region::Padding kNoPadding{ 0.0f, 0.0f, 0.0f, 0.0f };
	constexpr Util::Region::StabilizerPolicy kPolicy{ 3, 5, 0.75f };

	/** @brief The eight corners of a world-space axis-aligned box. */
	std::array<float3, 8> BoxCorners(const float3& a_min, const float3& a_max)
	{
		std::array<float3, 8> corners{};
		size_t index = 0;
		for (const float x : { a_min.x, a_max.x })
			for (const float y : { a_min.y, a_max.y })
				for (const float z : { a_min.z, a_max.z })
					corners[index++] = float3{ x, y, z };
		return corners;
	}

	/**
	 * @brief Row-vector world-to-clip matrix of a camera at the origin looking down +Z, 90 degrees
	 *        vertical and square pixels, so a point at depth z lands on NDC (x/z, y/z).
	 *        Written out rather than built from SimpleMath's Create* helpers: those are the
	 *        right-handed pair, and the engine hands the pass left-handed matrices (clip.w is
	 *        view-space depth), so building the camera from them would test the wrong convention.
	 */
	DirectX::SimpleMath::Matrix ViewProj()
	{
		constexpr float nearZ = 1.0f, farZ = 100.0f;
		constexpr float depth = farZ / (farZ - nearZ);
		return DirectX::SimpleMath::Matrix(
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, depth, 1.0f,
			0.0f, 0.0f, -nearZ * depth, 0.0f);
	}

	/** @brief An active crop with the same rect in both eyes. */
	StereoRegion ActiveRegion(const PixelRegion& a_region)
	{
		StereoRegion region;
		region.active = true;
		region.eye.fill(a_region);
		return region;
	}

	/** @brief Builds a crop from bounds with the test padding and alignment. */
	PixelRegion Crop(const ScreenBounds& a_bounds)
	{
		return Util::Region::PixelRegionFromBounds(a_bounds, kWidth, kHeight, kPadding, kAlignmentPixels);
	}

	/** @brief Runs the stabiliser for one whole shrink window of the same candidate. */
	void RunWindow(RegionStabilizer& a_stabilizer, const StereoRegion& a_candidate)
	{
		for (uint32_t frame = 0; frame < kPolicy.shrinkWindowFrames; ++frame)
			a_stabilizer.Update(a_candidate, kWidth, kHeight);
	}
}

TEST_CASE("ProjectToScreen maps the view centre to the centre and y downwards", "[region]")
{
	const auto viewProj = ViewProj();
	float2 point;

	REQUIRE(Util::Region::ProjectToScreen(viewProj, float3{ 0.0f, 0.0f, 10.0f }, point));
	REQUIRE(point.x == Catch::Approx(0.5f));
	REQUIRE(point.y == Catch::Approx(0.5f));

	REQUIRE(Util::Region::ProjectToScreen(viewProj, float3{ 1.0f, 0.0f, 10.0f }, point));
	REQUIRE(point.x == Catch::Approx(0.55f));

	REQUIRE(Util::Region::ProjectToScreen(viewProj, float3{ 0.0f, 1.0f, 10.0f }, point));
	REQUIRE(point.y == Catch::Approx(0.45f));
}

TEST_CASE("ProjectToScreen rejects points at or behind the eye plane", "[region]")
{
	const auto viewProj = ViewProj();
	float2 point;

	REQUIRE_FALSE(Util::Region::ProjectToScreen(viewProj, float3{ 0.0f, 0.0f, -10.0f }, point));
	REQUIRE_FALSE(Util::Region::ProjectToScreen(viewProj, float3{ 0.0f, 0.0f, 0.0f }, point));
}

TEST_CASE("ProjectBounds reports the box a target covers", "[region]")
{
	ScreenBounds bounds;

	REQUIRE(Util::Region::ProjectBounds(ViewProj(), BoxCorners({ -2.0f, -1.0f, 10.0f }, { 2.0f, 1.0f, 20.0f }), bounds) == ProjectionResult::kVisible);
	REQUIRE(bounds.minX == Catch::Approx(0.4f));
	REQUIRE(bounds.maxX == Catch::Approx(0.6f));
	REQUIRE(bounds.minY == Catch::Approx(0.45f));
	REQUIRE(bounds.maxY == Catch::Approx(0.55f));
}

TEST_CASE("ProjectBounds clamps a box that runs off the view", "[region]")
{
	ScreenBounds bounds;

	REQUIRE(Util::Region::ProjectBounds(ViewProj(), BoxCorners({ -20.0f, -1.0f, 10.0f }, { -2.0f, 1.0f, 20.0f }), bounds) == ProjectionResult::kVisible);
	REQUIRE(bounds.minX == Catch::Approx(0.0f));
	REQUIRE(bounds.maxX == Catch::Approx(0.45f));
}

TEST_CASE("ProjectBounds reports an offscreen box when it misses the view or lies wholly behind the eye", "[region]")
{
	ScreenBounds bounds;

	REQUIRE(Util::Region::ProjectBounds(ViewProj(), BoxCorners({ -40.0f, -1.0f, 10.0f }, { -30.0f, 1.0f, 20.0f }), bounds) == ProjectionResult::kOffscreen);
	REQUIRE(Util::Region::ProjectBounds(ViewProj(), BoxCorners({ -1.0f, -1.0f, -11.0f }, { 1.0f, 1.0f, -9.0f }), bounds) == ProjectionResult::kOffscreen);
}

TEST_CASE("ProjectBounds reports a box straddling the eye plane as behind the eye", "[region]")
{
	ScreenBounds bounds;

	REQUIRE(Util::Region::ProjectBounds(ViewProj(), BoxCorners({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }), bounds) == ProjectionResult::kBehindEye);
}

TEST_CASE("ProjectBounds takes corners relative to the eye, not to the world origin", "[region]")
{
	// The engine's view-projection takes camera-relative input; composing the eye subtraction into
	// the matrix must match pre-subtracting the corners, even far from the origin.
	const float3 eye{ -79872.5f, 41216.25f, 1234.0f };
	const auto relative = BoxCorners({ -2.0f, -1.0f, 10.0f }, { 2.0f, 1.0f, 20.0f });
	std::array<float3, 8> world{};
	for (size_t i = 0; i < relative.size(); ++i)
		world[i] = relative[i] + eye;

	const auto shift = DirectX::SimpleMath::Matrix::CreateTranslation(-eye.x, -eye.y, -eye.z);
	ScreenBounds shifted, direct;
	REQUIRE(Util::Region::ProjectBounds(shift * ViewProj(), world, shifted) == ProjectionResult::kVisible);
	REQUIRE(Util::Region::ProjectBounds(ViewProj(), relative, direct) == ProjectionResult::kVisible);
	REQUIRE(shifted.minX == Catch::Approx(direct.minX));
	REQUIRE(shifted.minY == Catch::Approx(direct.minY));
	REQUIRE(shifted.maxX == Catch::Approx(direct.maxX));
	REQUIRE(shifted.maxY == Catch::Approx(direct.maxY));
	REQUIRE(direct.minX == Catch::Approx(0.4f));
}

TEST_CASE("ClampToFrame keeps a crop that fits and rejects one that cannot", "[region]")
{
	const PixelRegion crop{ 576, 320, 768, 448 };
	REQUIRE(Util::Region::ClampToFrame(crop, kWidth, kHeight).x == 576u);
	REQUIRE(Util::Region::ClampToFrame(crop, kWidth, kHeight).w == 768u);

	const auto trimmed = Util::Region::ClampToFrame(PixelRegion{ 1792, 960, 768, 448 }, kWidth, kHeight);
	REQUIRE(trimmed.x == 1792u);
	REQUIRE(trimmed.w == 128u);
	REQUIRE(trimmed.h == 120u);

	REQUIRE(Util::Region::ClampToFrame(PixelRegion{ 1920, 0, 64, 64 }, kWidth, kHeight).w == 0u);
	REQUIRE(Util::Region::ClampToFrame(PixelRegion{ 0, 1080, 64, 64 }, kWidth, kHeight).w == 0u);
	REQUIRE(Util::Region::ClampToFrame(PixelRegion{ 0, 0, 0, 0 }, kWidth, kHeight).w == 0u);
	REQUIRE(Util::Region::ClampToFrame(crop, 0, kHeight).w == 0u);
}

TEST_CASE("Contains sees a crop inside another and excludes one that overhangs", "[region]")
{
	const PixelRegion outer{ 512, 256, 768, 576 };
	REQUIRE(Util::Region::Contains(outer, PixelRegion{ 576, 320, 512, 384 }));
	REQUIRE(Util::Region::Contains(outer, outer));
	REQUIRE_FALSE(Util::Region::Contains(outer, PixelRegion{ 1216, 256, 256, 256 }));
	REQUIRE_FALSE(Util::Region::Contains(outer, PixelRegion{ 512, 256, 1024, 576 }));
}

TEST_CASE("UnionRegion is the smallest crop covering both inputs", "[region]")
{
	const auto unioned = Util::Region::UnionRegion(PixelRegion{ 512, 256, 768, 576 }, PixelRegion{ 1216, 256, 256, 256 });
	REQUIRE(unioned.x == 512u);
	REQUIRE(unioned.y == 256u);
	REQUIRE(unioned.w == 960u);
	REQUIRE(unioned.h == 576u);
	REQUIRE(Util::Region::RectArea(unioned) == Catch::Approx(960.0f * 576.0f));
}

TEST_CASE("PixelRegionFromBounds keeps a non-empty crop at least one alignment step wide", "[region]")
{
	for (const ScreenBounds bounds : { ScreenBounds{ 0.35f, 0.35f, 0.65f, 0.65f },
			 ScreenBounds{ 0.02f, 0.02f, 0.05f, 0.05f }, ScreenBounds{ 0.95f, 0.95f, 0.999f, 0.999f } }) {
		const auto region = Crop(bounds);
		REQUIRE(region.w >= kAlignmentPixels);
		REQUIRE(region.h >= kAlignmentPixels);
	}
}

TEST_CASE("PixelRegionFromBounds aligns the origin but lets the frame bound the extent", "[region]")
{
	const auto region = Crop(ScreenBounds{ 0.35f, 0.02f, 0.65f, 0.98f });

	REQUIRE(region.x == 576u);
	REQUIRE(region.y == 0u);
	REQUIRE(region.w == 768u);
	REQUIRE(region.h == kHeight);
}

TEST_CASE("PixelRegionFromBounds pads, aligns and clamps the crop", "[region]")
{
	const auto region = Crop(ScreenBounds{ 0.35f, 0.35f, 0.65f, 0.65f });

	REQUIRE(region.x == 576u);
	REQUIRE(region.y == 320u);
	REQUIRE(region.w == 768u);
	REQUIRE(region.h == 448u);
}

TEST_CASE("PixelRegionFromBounds keeps the crop inside the frame and around the target", "[region]")
{
	for (const ScreenBounds bounds : { ScreenBounds{ 0.02f, 0.02f, 0.05f, 0.05f }, ScreenBounds{ 0.95f, 0.95f, 0.999f, 0.999f },
			 ScreenBounds{ 0.4f, 0.4f, 0.9f, 0.9f } }) {
		const auto region = Crop(bounds);

		REQUIRE(region.w >= 1u);
		REQUIRE(region.h >= 1u);
		REQUIRE(region.x + region.w <= kWidth);
		REQUIRE(region.y + region.h <= kHeight);
		REQUIRE(static_cast<float>(region.x) <= bounds.minX * static_cast<float>(kWidth));
		REQUIRE(static_cast<float>(region.y) <= bounds.minY * static_cast<float>(kHeight));
		REQUIRE(static_cast<float>(region.x + region.w) >= bounds.maxX * static_cast<float>(kWidth));
		REQUIRE(static_cast<float>(region.y + region.h) >= bounds.maxY * static_cast<float>(kHeight));
		REQUIRE(region.w < kWidth);
	}
}

TEST_CASE("PixelRegionFromBounds returns nothing for a degenerate frame size", "[region]")
{
	REQUIRE(Util::Region::PixelRegionFromBounds(ScreenBounds{ 0.4f, 0.4f, 0.6f, 0.6f }, 0, kHeight, kPadding).w == 0u);
	REQUIRE(Util::Region::PixelRegionFromBounds(ScreenBounds{ 0.4f, 0.4f, 0.6f, 0.6f }, kWidth, 0, kPadding).h == 0u);
}

TEST_CASE("RegionChanged ignores drift inside the tolerance band", "[region]")
{
	const StereoRegion previous = ActiveRegion(PixelRegion{ 640, 320, 512, 384 });
	const auto tolerance = static_cast<uint32_t>(kTolerancePixels);

	REQUIRE_FALSE(Util::Region::RegionChanged(previous, previous, kTolerancePixels));
	REQUIRE_FALSE(Util::Region::RegionChanged(ActiveRegion(PixelRegion{ 640 + tolerance, 320, 512, 384 }), previous, kTolerancePixels));
	REQUIRE(Util::Region::RegionChanged(ActiveRegion(PixelRegion{ 640 + tolerance + 1, 320, 512, 384 }), previous, kTolerancePixels));
	REQUIRE(Util::Region::RegionChanged(ActiveRegion(PixelRegion{ 640, 320, 512, 384 + tolerance + 1 }), previous, kTolerancePixels));
}

TEST_CASE("RegionChanged fires when the crop appears or disappears", "[region]")
{
	const StereoRegion inactive;
	const StereoRegion active = ActiveRegion(PixelRegion{ 640, 320, 512, 384 });

	REQUIRE(Util::Region::RegionChanged(active, inactive, kTolerancePixels));
	REQUIRE(Util::Region::RegionChanged(inactive, active, kTolerancePixels));
	REQUIRE_FALSE(Util::Region::RegionChanged(inactive, inactive, kTolerancePixels));
}

TEST_CASE("RegionChanged compares every eye independently", "[region]")
{
	StereoRegion previous = ActiveRegion(PixelRegion{ 0, 0, 960, 1080 });
	StereoRegion current = previous;
	current.eye[0] = PixelRegion{ 64, 128, 512, 512 };

	REQUIRE(Util::Region::RegionChanged(current, previous, kTolerancePixels));
}

TEST_CASE("RegionStabilizer adopts the first candidate and anchors while it stays inside", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	const auto held = ActiveRegion(PixelRegion{ 512, 256, 768, 576 });
	REQUIRE(stabilizer.Update(held, kWidth, kHeight).eye[0].x == 512u);

	const auto jitter = ActiveRegion(PixelRegion{ 576, 320, 512, 384 });
	const auto out = stabilizer.Update(jitter, kWidth, kHeight);
	REQUIRE(out.active);
	REQUIRE(out.eye[0].x == 512u);
	REQUIRE(out.eye[0].y == 256u);
	REQUIRE(out.eye[0].w == 768u);
	REQUIRE(out.eye[0].h == 576u);
}

TEST_CASE("RegionStabilizer Reset forgets the held crop so the next candidate is adopted fresh", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 512, 256, 768, 576 }), kWidth, kHeight);
	REQUIRE(stabilizer.held.active);

	stabilizer.Reset();
	REQUIRE_FALSE(stabilizer.held.active);

	const auto next = stabilizer.Update(ActiveRegion(PixelRegion{ 64, 64, 256, 256 }), kWidth, kHeight);
	REQUIRE(next.active);
	REQUIRE(next.eye[0].x == 64u);
	REQUIRE(next.eye[0].w == 256u);
}

TEST_CASE("RegionStabilizer grows by union when the candidate leaves the held crop", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 512, 256, 768, 576 }), kWidth, kHeight);

	const auto grown = stabilizer.Update(ActiveRegion(PixelRegion{ 1216, 256, 256, 256 }), kWidth, kHeight);
	REQUIRE(grown.eye[0].x == 512u);
	REQUIRE(grown.eye[0].y == 256u);
	REQUIRE(grown.eye[0].w == 960u);
	REQUIRE(grown.eye[0].h == 576u);
}

TEST_CASE("RegionStabilizer shrinks to the envelope only after the window and only past the area fraction", "[region][stabilize]")
{
	const auto bigCrop = ActiveRegion(PixelRegion{ 256, 128, 1024, 768 });
	const auto smallCrop = ActiveRegion(PixelRegion{ 640, 384, 256, 256 });

	RegionStabilizer stabilizer{ kPolicy };
	REQUIRE(stabilizer.Update(bigCrop, kWidth, kHeight).eye[0].w == 1024u);
	for (uint32_t frame = 0; frame + 1 < kPolicy.shrinkWindowFrames; ++frame)
		REQUIRE(stabilizer.Update(smallCrop, kWidth, kHeight).eye[0].w == 1024u);
	const auto shrunk = stabilizer.Update(smallCrop, kWidth, kHeight);
	REQUIRE(shrunk.eye[0].x == 640u);
	REQUIRE(shrunk.eye[0].y == 384u);
	REQUIRE(shrunk.eye[0].w == 256u);
	REQUIRE(shrunk.eye[0].h == 256u);
}

TEST_CASE("RegionStabilizer does not shrink for a saving inside the area fraction", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 256, 128, 1024, 768 }), kWidth, kHeight);
	const auto mostOfHeld = ActiveRegion(PixelRegion{ 256, 128, 900, 700 });
	RunWindow(stabilizer, mostOfHeld);
	const auto out = stabilizer.Update(mostOfHeld, kWidth, kHeight);
	REQUIRE(out.eye[0].w == 1024u);
	REQUIRE(out.eye[0].h == 768u);
}

TEST_CASE("RegionStabilizer grows by union and shrinks to the same window's envelope", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 0, 0, 1024, 1024 }), kWidth, kHeight);

	const auto grown = stabilizer.Update(ActiveRegion(PixelRegion{ 1024, 0, 256, 256 }), kWidth, kHeight);
	REQUIRE(grown.eye[0].w == 1280u);
	REQUIRE(grown.eye[0].h == 1024u);
	const auto smallUnion = ActiveRegion(PixelRegion{ 0, 0, 1280, 256 });
	RunWindow(stabilizer, smallUnion);
	const auto shrunk = stabilizer.Update(smallUnion, kWidth, kHeight);
	REQUIRE(shrunk.eye[0].w == 1280u);
	REQUIRE(shrunk.eye[0].h == 256u);
}

TEST_CASE("RegionStabilizer holds a crop through an inactive gap and drops it after the hold", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 512, 256, 768, 576 }), kWidth, kHeight);

	const StereoRegion inactive;
	for (uint32_t frame = 0; frame < kPolicy.holdFrames; ++frame) {
		const auto out = stabilizer.Update(inactive, kWidth, kHeight);
		REQUIRE(out.active);
		REQUIRE(out.eye[0].x == 512u);
	}
	REQUIRE_FALSE(stabilizer.Update(inactive, kWidth, kHeight).active);
}

TEST_CASE("RegionStabilizer continues the same crop when the candidate returns inside the hold", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	const auto crop = ActiveRegion(PixelRegion{ 512, 256, 768, 576 });
	stabilizer.Update(crop, kWidth, kHeight);

	const StereoRegion inactive;
	for (uint32_t frame = 0; frame < kPolicy.holdFrames / 2; ++frame)
		stabilizer.Update(inactive, kWidth, kHeight);

	const auto resumed = stabilizer.Update(ActiveRegion(PixelRegion{ 576, 320, 512, 384 }), kWidth, kHeight);
	REQUIRE(resumed.active);
	REQUIRE(resumed.eye[0].x == 512u);
	REQUIRE(resumed.eye[0].w == 768u);

	for (uint32_t frame = 0; frame < kPolicy.holdFrames; ++frame)
		REQUIRE(stabilizer.Update(inactive, kWidth, kHeight).active);
}

TEST_CASE("RegionStabilizer adopts a new crop fresh once the hold has expired", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 512, 256, 768, 576 }), kWidth, kHeight);
	const StereoRegion inactive;
	for (uint32_t frame = 0; frame <= kPolicy.holdFrames; ++frame)
		stabilizer.Update(inactive, kWidth, kHeight);

	const auto fresh = ActiveRegion(PixelRegion{ 64, 64, 256, 256 });
	const auto out = stabilizer.Update(fresh, kWidth, kHeight);
	REQUIRE(out.active);
	REQUIRE(out.eye[0].x == 64u);
	REQUIRE(out.eye[0].y == 64u);
	REQUIRE(out.eye[0].w == 256u);
}

TEST_CASE("RegionStabilizer keeps the eyes independent", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 0, 0, 512, 512 }), kWidth, kHeight);

	StereoRegion candidate;
	candidate.active = true;
	candidate.eye[0] = PixelRegion{ 64, 64, 256, 256 };
	candidate.eye[1] = PixelRegion{ 384, 384, 512, 512 };
	const auto out = stabilizer.Update(candidate, kWidth, kHeight);
	REQUIRE(out.eye[0].x == 0u);
	REQUIRE(out.eye[0].w == 512u);
	REQUIRE(out.eye[1].x == 0u);
	REQUIRE(out.eye[1].w == 896u);
}

TEST_CASE("RegionStabilizer clamps a grown crop to the frame", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 1536, 768, 384, 312 }), kWidth, kHeight);

	const auto out = stabilizer.Update(ActiveRegion(PixelRegion{ 1856, 1024, 256, 256 }), kWidth, kHeight);
	REQUIRE(out.eye[0].x + out.eye[0].w == kWidth);
	REQUIRE(out.eye[0].y + out.eye[0].h == kHeight);
	REQUIRE(out.eye[0].w == 384u);
	REQUIRE(out.eye[0].h == 312u);
}

TEST_CASE("RegionStabilizer clamps a shrunk crop to the frame", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 0, 0, 1024, 1024 }), kWidth, kHeight);

	const auto overhang = ActiveRegion(PixelRegion{ 1856, 1024, 256, 256 });
	RunWindow(stabilizer, overhang);
	REQUIRE(stabilizer.held.eye[0].x + stabilizer.held.eye[0].w <= kWidth);
	REQUIRE(stabilizer.held.eye[0].y + stabilizer.held.eye[0].h <= kHeight);
	REQUIRE(stabilizer.held.eye[0].w == 64u);
	REQUIRE(stabilizer.held.eye[0].h == 56u);
}

TEST_CASE("RegionStabilizer restarts the hold when the frame extent changes", "[region][stabilize]")
{
	RegionStabilizer stabilizer{ kPolicy };
	stabilizer.Update(ActiveRegion(PixelRegion{ 512, 256, 768, 576 }), kWidth, kHeight);

	const auto resized = stabilizer.Update(ActiveRegion(PixelRegion{ 64, 64, 256, 256 }), kWidth / 2, kHeight);
	REQUIRE(resized.eye[0].x == 64u);
	REQUIRE(resized.eye[0].w == 256u);
}

TEST_CASE("ShouldResetForRegion follows the change rule on kOnChange", "[region][reset]")
{
	const StereoRegion previous = ActiveRegion(PixelRegion{ 640, 320, 512, 384 });
	const StereoRegion inactive;
	const auto tolerance = static_cast<uint32_t>(kTolerancePixels);
	const std::array<StereoRegion, 6> cases{
		previous,
		ActiveRegion(PixelRegion{ 640 + tolerance, 320, 512, 384 }),
		ActiveRegion(PixelRegion{ 640 + tolerance + 1, 320, 512, 384 }),
		ActiveRegion(PixelRegion{ 640, 320, 512, 384 + tolerance + 1 }),
		inactive,
		ActiveRegion(PixelRegion{ 0, 0, 960, 1080 }),
	};

	for (const auto& current : cases)
		REQUIRE(Util::Region::ShouldResetForRegion(ResetPolicy::kOnChange, current, previous, kTolerancePixels) ==
				Util::Region::RegionChanged(current, previous, kTolerancePixels));
	for (const auto& current : cases)
		REQUIRE(Util::Region::ShouldResetForRegion(ResetPolicy::kOnChange, current, inactive, kTolerancePixels) ==
				Util::Region::RegionChanged(current, inactive, kTolerancePixels));
}

TEST_CASE("ShouldResetForRegion never resets for a crop change on kNever", "[region][reset]")
{
	const StereoRegion previous = ActiveRegion(PixelRegion{ 640, 320, 512, 384 });
	const StereoRegion inactive;
	const std::array<StereoRegion, 4> cases{
		previous,
		ActiveRegion(PixelRegion{ 640 + 4096, 320, 512, 384 }),
		inactive,
		ActiveRegion(PixelRegion{ 0, 0, 960, 1080 }),
	};
	for (const auto& current : cases)
		REQUIRE_FALSE(Util::Region::ShouldResetForRegion(ResetPolicy::kNever, current, previous, kTolerancePixels));
}

TEST_CASE("AreaFraction is the share of the frame a bounds rect covers", "[region][bounds]")
{
	REQUIRE(Util::Region::AreaFraction(ScreenBounds{ 0.0f, 0.0f, 1.0f, 1.0f }) == Catch::Approx(1.0f));
	REQUIRE(Util::Region::AreaFraction(ScreenBounds{ 0.25f, 0.0f, 0.75f, 1.0f }) == Catch::Approx(0.5f));
	REQUIRE(Util::Region::AreaFraction(ScreenBounds{ 0.25f, 0.25f, 0.75f, 0.5f }) == Catch::Approx(0.5f * 0.25f));
	REQUIRE(Util::Region::AreaFraction(ScreenBounds{ 0.5f, 0.5f, 0.5f, 0.5f }) == Catch::Approx(0.0f));
}

TEST_CASE("NormalizedCenterDistance is zero at the frame centre and one at a corner", "[region][bounds]")
{
	const auto centred = [](float a_halfWidth, float a_halfHeight) {
		return ScreenBounds{ 0.5f - a_halfWidth, 0.5f - a_halfHeight, 0.5f + a_halfWidth, 0.5f + a_halfHeight };
	};
	REQUIRE(Util::Region::NormalizedCenterDistance(centred(0.1f, 0.2f)) == Catch::Approx(0.0f));
	REQUIRE(Util::Region::NormalizedCenterDistance(ScreenBounds{ 1.0f, 1.0f, 1.0f, 1.0f }) == Catch::Approx(1.0f));
	REQUIRE(Util::Region::NormalizedCenterDistance(ScreenBounds{ 0.0f, 0.0f, 0.0f, 0.0f }) == Catch::Approx(1.0f));
	REQUIRE(Util::Region::NormalizedCenterDistance(ScreenBounds{ 1.0f, 0.0f, 1.0f, 0.0f }) == Catch::Approx(1.0f));
	REQUIRE(Util::Region::NormalizedCenterDistance(ScreenBounds{ 0.0f, 1.0f, 0.0f, 1.0f }) == Catch::Approx(1.0f));
	REQUIRE(Util::Region::NormalizedCenterDistance(ScreenBounds{ 0.1f, 0.45f, 0.2f, 0.55f }) ==
			Catch::Approx(Util::Region::NormalizedCenterDistance(ScreenBounds{ 0.8f, 0.45f, 0.9f, 0.55f })));
	REQUIRE(Util::Region::NormalizedCenterDistance(ScreenBounds{ 0.45f, 0.1f, 0.55f, 0.2f }) ==
			Catch::Approx(Util::Region::NormalizedCenterDistance(ScreenBounds{ 0.45f, 0.8f, 0.55f, 0.9f })));
}

TEST_CASE("UnionNonEmpty ignores an empty rect instead of stretching to the origin", "[region][merge]")
{
	const Util::Subrect::PixelRegion rect{ 300, 200, 100, 50 };
	REQUIRE(Util::Region::UnionNonEmpty(Util::Region::kEmptyRegion, rect).x == rect.x);
	REQUIRE(Util::Region::UnionNonEmpty(rect, Util::Region::kEmptyRegion).w == rect.w);
	const auto both = Util::Region::UnionNonEmpty(rect, { 500, 100, 100, 100 });
	REQUIRE(both.x == 300);
	REQUIRE(both.y == 100);
	REQUIRE(both.w == 300);
	REQUIRE(both.h == 150);
}

TEST_CASE("TryMergeRegions grows a crop only while it stays under the area cap", "[region][merge]")
{
	constexpr uint32_t width = 1000, height = 1000;
	Util::Region::StereoRegion base;
	base.active = true;
	base.eye = { Util::Subrect::PixelRegion{ 0, 0, 200, 200 }, Util::Subrect::PixelRegion{ 0, 0, 200, 200 } };

	Util::Region::StereoRegion adjacent;
	adjacent.eye = { Util::Subrect::PixelRegion{ 200, 0, 200, 200 }, Util::Region::kEmptyRegion };
	REQUIRE(Util::Region::TryMergeRegions(base, adjacent, width, height, 2, 0.5f));
	REQUIRE(base.eye[0].w == 400);
	REQUIRE(base.eye[1].w == 200);

	Util::Region::StereoRegion distant;
	distant.eye = { Util::Subrect::PixelRegion{ 800, 800, 200, 200 }, Util::Region::kEmptyRegion };
	const auto before = base.eye[0];
	REQUIRE_FALSE(Util::Region::TryMergeRegions(base, distant, width, height, 2, 0.5f));
	REQUIRE(base.eye[0].w == before.w);
	REQUIRE(base.eye[0].h == before.h);
}

TEST_CASE("TryMergeRegions leaves an eye the base does not crop uncropped", "[region][merge]")
{
	constexpr uint32_t width = 1000, height = 1000;
	Util::Region::StereoRegion base;
	base.active = true;
	base.eye = { Util::Subrect::PixelRegion{ 0, 0, 200, 200 }, Util::Subrect::PixelRegion{ 0, 0, width, height } };
	Util::Region::StereoRegion addition;
	addition.eye = { Util::Subrect::PixelRegion{ 200, 0, 100, 100 }, Util::Subrect::PixelRegion{ 500, 500, 100, 100 } };
	REQUIRE(Util::Region::TryMergeRegions(base, addition, width, height, 2, 0.5f));
	REQUIRE(base.eye[1].w == width);
	REQUIRE(base.eye[1].h == height);
}

TEST_CASE("MatchEyeSizes gives both eyes the larger size and keeps each position", "[region][stereo]")
{
	StereoRegion region;
	region.active = true;
	region.eye = { PixelRegion{ 128, 64, 192, 576 }, PixelRegion{ 64, 128, 256, 512 } };
	Util::Region::MatchEyeSizes(region, kWidth, kHeight);
	for (const auto& eye : region.eye) {
		REQUIRE(eye.w == 256u);
		REQUIRE(eye.h == 576u);
	}
	REQUIRE(region.eye[0].x == 128u);
	REQUIRE(region.eye[1].y == 128u);
}

TEST_CASE("MatchEyeSizes slides a crop back inside the frame instead of shrinking it", "[region][stereo]")
{
	StereoRegion region;
	region.active = true;
	region.eye = { PixelRegion{ kWidth - 128, 0, 128, 256 }, PixelRegion{ 64, 0, 256, 256 } };
	Util::Region::MatchEyeSizes(region, kWidth, kHeight);
	REQUIRE(region.eye[0].w == 256u);
	REQUIRE(region.eye[0].x == kWidth - 256u);
	REQUIRE(region.eye[1].x == 64u);
}

TEST_CASE("MatchEyeSizes leaves a region with an empty eye untouched", "[region][stereo]")
{
	StereoRegion region;
	region.active = true;
	region.eye = { PixelRegion{ 128, 64, 192, 576 }, Util::Region::kEmptyRegion };
	Util::Region::MatchEyeSizes(region, kWidth, kHeight);
	REQUIRE(region.eye[0].w == 192u);
	REQUIRE(region.eye[1].w == 0u);
}

TEST_CASE("MatchEyeSizes does not enlarge a crop to match a whole-frame eye", "[region][stereo]")
{
	StereoRegion region;
	region.active = true;
	region.eye = { PixelRegion{ 128, 64, 192, 576 }, PixelRegion{ 0, 0, kWidth, kHeight } };
	Util::Region::MatchEyeSizes(region, kWidth, kHeight);
	REQUIRE(region.eye[0].w == 192u);
	REQUIRE(region.eye[1].w == kWidth);
}

TEST_CASE("CopyCropToUnprojectedEyes gives a missed eye the projecting eye's crop", "[region][stereo]")
{
	StereoRegion region;
	region.active = true;
	region.eye = { PixelRegion{ 128, 64, 192, 576 }, PixelRegion{ 0, 0, kWidth, kHeight } };
	Util::Region::CopyCropToUnprojectedEyes(region, { true, false }, kWidth, kHeight);
	REQUIRE(region.eye[1].x == 128u);
	REQUIRE(region.eye[1].w == 192u);
	REQUIRE(region.eye[0].w == 192u);
}

TEST_CASE("CopyCropToUnprojectedEyes does not copy a whole-frame donor", "[region][stereo]")
{
	StereoRegion region;
	region.active = true;
	region.eye = { PixelRegion{ 0, 0, kWidth, kHeight }, PixelRegion{ 0, 0, kWidth, kHeight } };
	Util::Region::CopyCropToUnprojectedEyes(region, { true, false }, kWidth, kHeight);
	REQUIRE(region.eye[1].w == kWidth);
	Util::Region::CopyCropToUnprojectedEyes(region, { false, false }, kWidth, kHeight);
	REQUIRE(region.eye[0].w == kWidth);
}

TEST_CASE("BoundsFromUV maps a full-frame UV rect to the whole screen", "[region][uv]")
{
	const auto bounds = Util::Region::BoundsFromUV(UVRegion{});
	REQUIRE(bounds.minX == Catch::Approx(0.0f));
	REQUIRE(bounds.minY == Catch::Approx(0.0f));
	REQUIRE(bounds.maxX == Catch::Approx(1.0f));
	REQUIRE(bounds.maxY == Catch::Approx(1.0f));
}

TEST_CASE("BoundsFromUV feeds PixelRegionFromBounds the same rect in pixels", "[region][uv]")
{
	const auto region = Util::Region::PixelRegionFromBounds(
		Util::Region::BoundsFromUV(UVRegion{ 0.25f, 0.5f, 0.5f, 0.25f }), kWidth, kHeight, kNoPadding, Util::Region::kNoPixelAlignment);
	REQUIRE(region.x == 480u);
	REQUIRE(region.y == 540u);
	REQUIRE(region.w == 960u);
	REQUIRE(region.h == 270u);

	const auto fullFrame = Util::Region::PixelRegionFromBounds(
		Util::Region::BoundsFromUV(UVRegion{}), kWidth, kHeight, kNoPadding, Util::Region::kNoPixelAlignment);
	REQUIRE(fullFrame.x == 0u);
	REQUIRE(fullFrame.y == 0u);
	REQUIRE(fullFrame.w == kWidth);
	REQUIRE(fullFrame.h == kHeight);
}

TEST_CASE("Intersect is the overlap of two crops", "[region][clip]")
{
	const auto overlap = Util::Region::Intersect(PixelRegion{ 0, 0, 100, 100 }, PixelRegion{ 50, 50, 100, 100 });
	REQUIRE(overlap.x == 50u);
	REQUIRE(overlap.y == 50u);
	REQUIRE(overlap.w == 50u);
	REQUIRE(overlap.h == 50u);

	const auto inner = Util::Region::Intersect(PixelRegion{ 0, 0, 100, 100 }, PixelRegion{ 20, 20, 10, 10 });
	REQUIRE(inner.x == 20u);
	REQUIRE(inner.y == 20u);
	REQUIRE(inner.w == 10u);
	REQUIRE(inner.h == 10u);
}

TEST_CASE("Intersect is empty for disjoint crops and edges that only touch", "[region][clip]")
{
	REQUIRE(Util::Region::Intersect(PixelRegion{ 0, 0, 10, 10 }, PixelRegion{ 50, 50, 10, 10 }).w == 0u);
	REQUIRE(Util::Region::Intersect(PixelRegion{ 0, 0, 10, 10 }, PixelRegion{ 10, 0, 10, 10 }).w == 0u);
	REQUIRE(Util::Region::Intersect(PixelRegion{ 0, 10, 10, 10 }, PixelRegion{ 0, 0, 10, 10 }).w == 0u);
}

TEST_CASE("Intersect is empty when either input is empty", "[region][clip]")
{
	REQUIRE(Util::Region::Intersect(Util::Region::kEmptyRegion, PixelRegion{ 0, 0, 100, 100 }).w == 0u);
	REQUIRE(Util::Region::Intersect(PixelRegion{ 0, 0, 100, 100 }, Util::Region::kEmptyRegion).w == 0u);
	REQUIRE(Util::Region::Intersect(Util::Region::kEmptyRegion, Util::Region::kEmptyRegion).w == 0u);
}

TEST_CASE("Intersect does not wrap the right and bottom edges near UINT32_MAX", "[region][clip]")
{
	constexpr uint32_t max = std::numeric_limits<uint32_t>::max();
	const auto region = Util::Region::Intersect(PixelRegion{ max - 10, 0, 100, 100 }, PixelRegion{ max - 20, 0, 100, 100 });
	REQUIRE(region.x == max - 10);
	REQUIRE(region.w == 90u);
	REQUIRE(region.h == 100u);
}

TEST_CASE("ClipRegion clips each eye independently", "[region][clip]")
{
	StereoRegion focus = ActiveRegion(PixelRegion{ 0, 0, 100, 100 });
	focus.eye[1] = PixelRegion{ 200, 0, 100, 100 };
	StereoRegion clip;
	clip.active = true;
	clip.eye[0] = PixelRegion{ 50, 50, 100, 100 };

	Util::Region::ClipRegion(focus, clip);
	REQUIRE(focus.eye[0].x == 50u);
	REQUIRE(focus.eye[0].y == 50u);
	REQUIRE(focus.eye[0].w == 50u);
	REQUIRE(focus.eye[0].h == 50u);
	REQUIRE(focus.eye[1].x == 200u);
	REQUIRE(focus.eye[1].w == 100u);
}

TEST_CASE("ClipRegion falls back to the clip eye when the focus misses it", "[region][clip]")
{
	StereoRegion focus = ActiveRegion(PixelRegion{ 500, 500, 100, 100 });
	StereoRegion clip;
	clip.active = true;
	clip.eye.fill(PixelRegion{ 0, 0, 100, 100 });

	Util::Region::ClipRegion(focus, clip);
	REQUIRE(focus.eye[0].x == 0u);
	REQUIRE(focus.eye[0].w == 100u);
	REQUIRE(focus.eye[1].x == 0u);
	REQUIRE(focus.eye[1].w == 100u);
}

TEST_CASE("ClipRegion gives an uncropped focus eye the clip eye", "[region][clip]")
{
	StereoRegion focus;
	focus.active = true;
	StereoRegion clip;
	clip.active = true;
	clip.eye.fill(PixelRegion{ 10, 20, 30, 40 });

	Util::Region::ClipRegion(focus, clip);
	REQUIRE(focus.eye[0].x == 10u);
	REQUIRE(focus.eye[0].y == 20u);
	REQUIRE(focus.eye[0].w == 30u);
	REQUIRE(focus.eye[0].h == 40u);
	REQUIRE(focus.eye[1].x == 10u);
	REQUIRE(focus.eye[1].w == 30u);
}

TEST_CASE("ClipRegion leaves a focus eye whose clip eye is empty", "[region][clip]")
{
	StereoRegion focus = ActiveRegion(PixelRegion{ 100, 100, 50, 50 });
	StereoRegion clip;
	clip.active = true;

	Util::Region::ClipRegion(focus, clip);
	REQUIRE(focus.eye[0].x == 100u);
	REQUIRE(focus.eye[0].y == 100u);
	REQUIRE(focus.eye[0].w == 50u);
	REQUIRE(focus.eye[1].x == 100u);
	REQUIRE(focus.eye[1].w == 50u);
}

TEST_CASE("ClipRegion changes nothing for an inactive clip", "[region][clip]")
{
	StereoRegion focus = ActiveRegion(PixelRegion{ 100, 100, 50, 50 });
	const auto before = focus;
	StereoRegion clip;
	clip.active = false;
	clip.eye.fill(PixelRegion{ 0, 0, 200, 200 });

	Util::Region::ClipRegion(focus, clip);
	REQUIRE(focus.active == before.active);
	REQUIRE(focus.eye[0].x == before.eye[0].x);
	REQUIRE(focus.eye[0].w == before.eye[0].w);
	REQUIRE(focus.eye[1].w == before.eye[1].w);
}

TEST_CASE("ClipRegion ignores the retained eyes of an inactive focus", "[region][clip]")
{
	StereoRegion focus = ActiveRegion(PixelRegion{ 0, 0, 100, 100 });
	focus.active = false;
	StereoRegion clip;
	clip.active = true;
	clip.eye.fill(PixelRegion{ 50, 50, 100, 100 });

	Util::Region::ClipRegion(focus, clip);
	REQUIRE(focus.active);
	REQUIRE(focus.eye[0].x == 50u);
	REQUIRE(focus.eye[0].w == 100u);
	REQUIRE(focus.eye[1].h == 100u);
}

TEST_CASE("ClipRegion makes the result active when the clip is active", "[region][clip]")
{
	StereoRegion focus;
	StereoRegion clip;
	clip.active = true;

	Util::Region::ClipRegion(focus, clip);
	REQUIRE(focus.active);

	const StereoRegion inactiveClip;
	StereoRegion untouched;
	Util::Region::ClipRegion(untouched, inactiveClip);
	REQUIRE_FALSE(untouched.active);
}
