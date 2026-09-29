// HLSL Unit Tests for Common/RegionOverlay.hlsli
#include "/Shaders/Common/RegionOverlay.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

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
[numthreads(1, 1, 1)] void TestRegionOverlayClampToFrame() {
	const uint4 rect = uint4(100, 50, 200, 100);
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(rect, uint2(1920, 1080)), rect);
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(0, 0, 3000, 4000), uint2(1920, 1080)), uint4(0, 0, 1920, 1080));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(1900, 1000, 500, 500), uint2(1920, 1080)), uint4(1900, 1000, 20, 80));
}

	/// @tags utility, regionoverlay
	[numthreads(1, 1, 1)] void TestRegionOverlayClampToFrameRejectsAnUnfittableRect()
{
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(10, 10, 100, 100), uint2(1920, 0)), uint4(0, 0, 0, 0));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(10, 10, 0, 100), uint2(1920, 1080)), uint4(0, 0, 0, 0));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(2000, 10, 100, 100), uint2(1920, 1080)), uint4(0, 0, 0, 0));
	ASSERT(AreEqual, RegionOverlay::ClampToFrame(uint4(10, 1080, 100, 100), uint2(1920, 1080)), uint4(0, 0, 0, 0));
}

/// @tags utility, regionoverlay
[numthreads(1, 1, 1)] void TestRegionOverlayOutlineOnlyPaintsOnlyTheOutline() {
	const uint4 rect = uint4(10, 20, 100, 50);
	const float3 color = float3(0.8, 0.6, 0.4);
	const float3 outline = float3(1.0, 1.0, 0.0);
	ASSERT(IsTrue, all(abs(RegionOverlay::OutlineOnly(color, uint2(60, 45), rect, outline, 3.0f) - color) < 1e-5));
	ASSERT(IsTrue, all(abs(RegionOverlay::OutlineOnly(color, uint2(0, 0), rect, outline, 3.0f) - color) < 1e-5));
	ASSERT(IsTrue, all(abs(RegionOverlay::OutlineOnly(color, uint2(10, 45), rect, outline, 3.0f) - outline) < 1e-5));
	ASSERT(IsTrue, all(abs(RegionOverlay::OutlineOnly(color, uint2(50, 69), rect, outline, 3.0f) - outline) < 1e-5));
}

	/// @tags utility, regionoverlay
	[numthreads(1, 1, 1)] void TestRegionOverlayOutlineOnlyLeavesAnEmptyOrThinRectAlone()
{
	const float3 color = float3(0.8, 0.6, 0.4);
	const float3 outline = float3(1.0, 1.0, 0.0);
	ASSERT(IsTrue, all(abs(RegionOverlay::OutlineOnly(color, uint2(0, 0), uint4(0, 0, 0, 0), outline, 3.0f) - color) < 1e-5));
	ASSERT(IsTrue, all(abs(RegionOverlay::OutlineOnly(color, uint2(10, 20), uint4(10, 20, 100, 50), outline, 0.0f) - color) < 1e-5));
}
