#define VR
#include "/Shaders/Common/WaterRefraction.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

[numthreads(1, 1, 1)] void TestWaterRefractionKeepsOddSizedEyeFootprintsSeparate() {
	float2 leftMin, leftMax, rightMin, rightMax;
	ASSERT(IsTrue, WaterRefraction::TryGetUVBounds(float2(101, 53), float2(51, 27), 0, leftMin, leftMax));
	ASSERT(IsTrue, WaterRefraction::TryGetUVBounds(float2(101, 53), float2(51, 27), 1, rightMin, rightMax));
	ASSERT(IsTrue, leftMax.x < 0.5 && rightMin.x >= 0.5);
	ASSERT(IsTrue, all(leftMin <= leftMax) && all(rightMin <= rightMax));
	ASSERT(IsTrue, WaterRefraction::ClampUV(0.75.xx, 0.25.xx, leftMin, leftMax).x == leftMax.x);
	ASSERT(IsTrue, WaterRefraction::ClampUV(0.25.xx, 0.75.xx, rightMin, rightMax).x == rightMin.x);
}

	[numthreads(1, 1, 1)] void TestWaterRefractionRejectsAnEmptyStereoEye()
{
	float2 lo, hi;
	ASSERT(IsFalse, WaterRefraction::TryGetUVBounds(float2(1, 100), float2(100, 100), 0, lo, hi));
	ASSERT(IsFalse, WaterRefraction::TryGetUVBounds(float2(100, 100), float2(1, 100), 1, lo, hi));
	ASSERT(IsTrue, all(lo == 0.0) && all(hi == 0.0));
}
