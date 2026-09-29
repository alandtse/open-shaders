// HLSL Unit Tests for Common/RegionOverlay.hlsli
#include "/Shaders/Common/RegionOverlay.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags utility, regionoverlay
[numthreads(1, 1, 1)] void TestRegionOverlayInsideCoverage() {
	const uint4 rect = uint4(10, 20, 100, 50);
	ASSERT(AreEqual, RegionOverlay::InsideCoverage(uint2(10, 20), rect), 1.0f);
	ASSERT(AreEqual, RegionOverlay::InsideCoverage(uint2(109, 69), rect), 1.0f);
	// Half-open on the far edge: x + width is the first pixel outside.
	ASSERT(AreEqual, RegionOverlay::InsideCoverage(uint2(110, 70), rect), 0.0f);
	ASSERT(AreEqual, RegionOverlay::InsideCoverage(uint2(9, 20), rect), 0.0f);
	ASSERT(AreEqual, RegionOverlay::InsideCoverage(uint2(10, 19), rect), 0.0f);
}

	/// @tags utility, regionoverlay
	[numthreads(1, 1, 1)] void TestRegionOverlayInsideCoverageIgnoresAnEmptyRect()
{
	ASSERT(AreEqual, RegionOverlay::InsideCoverage(uint2(10, 20), uint4(10, 20, 0, 50)), 0.0f);
	ASSERT(AreEqual, RegionOverlay::InsideCoverage(uint2(10, 20), uint4(10, 20, 100, 0)), 0.0f);
}

/// @tags utility, regionoverlay
[numthreads(1, 1, 1)] void TestRegionOverlayOutlineCoverage() {
	const uint4 rect = uint4(10, 20, 100, 50);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(10, 20), rect, 3.0f), 1.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(12, 30), rect, 3.0f), 1.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(13, 30), rect, 3.0f), 0.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(109, 45), rect, 3.0f), 1.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(106, 45), rect, 3.0f), 0.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(50, 69), rect, 3.0f), 1.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(50, 66), rect, 3.0f), 0.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(60, 45), rect, 3.0f), 0.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(9, 45), rect, 3.0f), 0.0f);
}

	/// @tags utility, regionoverlay
	[numthreads(1, 1, 1)] void TestRegionOverlayOutlineCoverageRejectsEmptyOrThinRegions()
{
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(10, 20), uint4(10, 20, 0, 50), 3.0f), 0.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(10, 20), uint4(10, 20, 100, 50), 0.0f), 0.0f);
	ASSERT(AreEqual, RegionOverlay::OutlineCoverage(uint2(10, 20), uint4(10, 20, 100, 50), -1.0f), 0.0f);
}

/// @tags utility, regionoverlay
[numthreads(1, 1, 1)] void TestRegionOverlayApplyDimsOutsideAndPaintsTheOutline() {
	const uint4 rect = uint4(10, 20, 100, 50);
	const float3 color = float3(0.8, 0.6, 0.4);
	const float3 outline = float3(0.0, 1.0, 0.0);
	ASSERT(IsTrue, all(abs(RegionOverlay::Apply(color, uint2(60, 45), rect, outline, 0.35, 3.0f) - color) < 1e-5));
	const float3 dimmed = RegionOverlay::Apply(color, uint2(0, 0), rect, outline, 0.35, 3.0f);
	ASSERT(IsTrue, all(abs(dimmed - color * 0.35) < 1e-5));
	ASSERT(IsTrue, all(abs(RegionOverlay::Apply(color, uint2(10, 45), rect, outline, 0.35, 3.0f) - outline) < 1e-5));
	ASSERT(IsTrue, all(abs(RegionOverlay::Apply(color, uint2(109, 45), rect, outline, 0.35, 3.0f) - outline) < 1e-5));
}

	/// @tags utility, regionoverlay
	[numthreads(1, 1, 1)] void TestRegionOverlayClampToFrame()
{
	const uint4 rect = uint4(100, 50, 200, 100);
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(rect, uint2(1920, 1080)), rect);
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(0, 0, 3000, 4000), uint2(1920, 1080)), uint4(0, 0, 1920, 1080));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(1900, 1000, 500, 500), uint2(1920, 1080)), uint4(1900, 1000, 20, 80));
}

/// @tags utility, regionoverlay
[numthreads(1, 1, 1)] void TestRegionOverlayClampToFrameRejectsAnUnfittableRect() {
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(10, 10, 100, 100), uint2(1920, 0)), uint4(0, 0, 0, 0));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(10, 10, 0, 100), uint2(1920, 1080)), uint4(0, 0, 0, 0));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(2000, 10, 100, 100), uint2(1920, 1080)), uint4(0, 0, 0, 0));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(10, 1080, 100, 100), uint2(1920, 1080)), uint4(0, 0, 0, 0));
}

	/// @tags utility, regionoverlay
	[numthreads(1, 1, 1)] void TestRegionOverlayApplyLeavesAnEmptyRectAlone()
{
	const float3 color = float3(0.8, 0.6, 0.4);
	const float3 result = RegionOverlay::Apply(color, uint2(0, 0), uint4(0, 0, 0, 0), float3(0.0, 1.0, 0.0), 0.0, 3.0f);
	ASSERT(IsTrue, all(abs(result - color) < 1e-5));
}
