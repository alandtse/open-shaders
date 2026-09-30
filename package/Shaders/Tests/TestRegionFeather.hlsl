// HLSL Unit Tests for Common/RegionFeather.hlsli
#include "/Shaders/Common/RegionFeather.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

static const float2 kFrame = float2(1000.0, 800.0);
static const float4 kCrop = float4(100.0, 100.0, 500.0, 400.0);
static const float4 kNoSubject = float4(0.0, 0.0, 0.0, 0.0);
static const float kTolerance = 1e-4;

/// @tags utility, regionfeather
[numthreads(1, 1, 1)] void TestRegionFeatherSideWidthStaysWithinBounds() {
	ASSERT(AreEqual, RegionFeather::SideWidth(10.0, 16.0, 96.0), 16.0);
	ASSERT(AreEqual, RegionFeather::SideWidth(40.0, 16.0, 96.0), 40.0);
	ASSERT(AreEqual, RegionFeather::SideWidth(200.0, 16.0, 96.0), 96.0);
	ASSERT(AreEqual, RegionFeather::SideWidth(-30.0, 16.0, 96.0), 16.0);
}

	/// @tags utility, regionfeather
	[numthreads(1, 1, 1)] void TestRegionFeatherIsZeroOutsideAndOneWellInside()
{
	ASSERT(AreEqual, RegionFeather::Weight(float2(99.5, 250.0), kCrop, kNoSubject, kFrame, 32.0, 16.0, 96.0), 0.0);
	ASSERT(AreEqual, RegionFeather::Weight(float2(300.0, 450.0), kCrop, kNoSubject, kFrame, 32.0, 16.0, 96.0), 0.0);
	ASSERT(AreEqual, RegionFeather::Weight(float2(300.0, 250.0), kCrop, kNoSubject, kFrame, 32.0, 16.0, 96.0), 1.0);
}

/// @tags utility, regionfeather
[numthreads(1, 1, 1)] void TestRegionFeatherWithoutASubjectUsesTheDefaultWidth() {
	const float weight = RegionFeather::Weight(float2(116.0, 250.0), kCrop, kNoSubject, kFrame, 32.0, 16.0, 96.0);
	ASSERT(IsTrue, abs(weight - 0.5) < kTolerance);
}

	/// @tags utility, regionfeather
	[numthreads(1, 1, 1)] void TestRegionFeatherBandSpansTheMarginToTheSubject()
{
	const float4 subject = float4(180.0, 180.0, 420.0, 320.0);
	const float halfway = RegionFeather::Weight(float2(140.0, 250.0), kCrop, subject, kFrame, 32.0, 16.0, 96.0);
	ASSERT(IsTrue, abs(halfway - 0.5) < kTolerance);
	ASSERT(AreEqual, RegionFeather::Weight(float2(180.0, 250.0), kCrop, subject, kFrame, 32.0, 16.0, 96.0), 1.0);
}

/// @tags utility, regionfeather
[numthreads(1, 1, 1)] void TestRegionFeatherNeverFallsBelowTheMinimumWidth() {
	const float4 flushSubject = float4(100.0, 100.0, 500.0, 400.0);
	const float weight = RegionFeather::Weight(float2(108.0, 250.0), kCrop, flushSubject, kFrame, 32.0, 16.0, 96.0);
	ASSERT(IsTrue, abs(weight - 0.5) < kTolerance);
}

	/// @tags utility, regionfeather
	[numthreads(1, 1, 1)] void TestRegionFeatherDoesNotFeatherAnEdgeOnTheFrameEdge()
{
	const float4 crop = float4(0.0, 100.0, 400.0, 400.0);
	ASSERT(AreEqual, RegionFeather::Weight(float2(0.5, 250.0), crop, kNoSubject, kFrame, 32.0, 16.0, 96.0), 1.0);
}
