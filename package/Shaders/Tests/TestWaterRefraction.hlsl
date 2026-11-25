#include "/Shaders/Common/WaterRefraction.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

[numthreads(1, 1, 1)] void TestWaterRefractionRejectsEmptyFootprint() {
	float2 lo, hi;
	ASSERT(IsFalse, WaterRefraction::TryGetUVBounds(float2(0, 100), float2(100, 100), 0, lo, hi));
	ASSERT(IsFalse, WaterRefraction::TryGetUVBounds(float2(100, 100), float2(100, 0), 0, lo, hi));
}

	[numthreads(1, 1, 1)] void TestWaterRefractionFiniteFallback()
{
	float2 value = WaterRefraction::ClampUV(float2(asfloat(0x7fc00000), 0.5), float2(0.25, 0.75), float2(0.1, 0.1), float2(0.9, 0.9));
	ASSERT(IsTrue, all(value == float2(0.25, 0.75)));
}

[numthreads(1, 1, 1)] void TestWaterRefractionRejectsNonFiniteFootprints() {
	float2 lo = 1.0, hi = 1.0;
	ASSERT(IsFalse, WaterRefraction::TryGetUVBounds(float2(asfloat(0x7f800000u), 100), float2(100, 100), 0, lo, hi));
	ASSERT(IsTrue, all(lo == 0.0) && all(hi == 0.0));
	ASSERT(IsFalse, WaterRefraction::TryGetUVBounds(float2(100, 100), float2(100, asfloat(0x7fc00000u)), 0, lo, hi));
	ASSERT(IsTrue, all(lo == 0.0) && all(hi == 0.0));
}

	[numthreads(1, 1, 1)] void TestWaterRefractionFallsBackFromInfinity()
{
	float2 value = WaterRefraction::ClampUV(float2(asfloat(((1u << 31) | 0x7f800000u)), 0.5), float2(asfloat(0x7fc00000u), 0.5), 0.1.xx, 0.9.xx);
	ASSERT(IsTrue, all(abs(value - 0.5.xx) < 1e-6));
}

[numthreads(1, 1, 1)] void TestWaterRefractionIntersectsColourAndDepthFootprints() {
	float2 lo, hi;
	ASSERT(IsTrue, WaterRefraction::TryGetUVBounds(float2(101, 53), float2(51, 27), 0, lo, hi));
	ASSERT(IsTrue, all(abs(lo - float2(0.5 / 51.0, 0.5 / 27.0)) < 1e-6));
	ASSERT(IsTrue, all(abs(hi - float2(50.5 / 51.0, 26.5 / 27.0)) < 1e-6));
	ASSERT(IsTrue, all(WaterRefraction::ClampUV(float2(-1, 2), 0.5.xx, lo, hi) == float2(lo.x, hi.y)));
}
