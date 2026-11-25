#include "/Shaders/Common/WaterFinite.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

[numthreads(1, 1, 1)] void TestWaterFiniteClassifiesIEEE754Values() {
	ASSERT(IsTrue, WaterFinite::IsFinite(asfloat(0x00000000u)));
	ASSERT(IsTrue, WaterFinite::IsFinite(asfloat((1u << 31))));
	ASSERT(IsTrue, WaterFinite::IsFinite(asfloat(0x00000001u)));
	ASSERT(IsTrue, WaterFinite::IsFinite(asfloat(0x7f7fffffu)));
	ASSERT(IsTrue, WaterFinite::IsFinite(asfloat(((1u << 31) | 0x7f7fffffu))));
	ASSERT(IsFalse, WaterFinite::IsFinite(asfloat(0x7f800000u)));
	ASSERT(IsFalse, WaterFinite::IsFinite(asfloat(((1u << 31) | 0x7f800000u))));
	ASSERT(IsFalse, WaterFinite::IsFinite(asfloat(0x7fc00000u)));
	ASSERT(IsFalse, WaterFinite::IsFinite(asfloat(((1u << 31) | 0x7fc00001u))));
}

	[numthreads(1, 1, 1)] void TestWaterFiniteVectorComponents()
{
	float infinity = asfloat(0x7f800000u);
	float nanValue = asfloat(0x7fc00000u);
	ASSERT(IsTrue, all(WaterFinite::IsFinite(float2(0.0, infinity)) == bool2(true, false)));
	ASSERT(IsTrue, all(WaterFinite::IsFinite(float3(nanValue, -1.0, infinity)) == bool3(false, true, false)));
	ASSERT(IsTrue, all(WaterFinite::IsFinite(float4(1.0, nanValue, infinity, -1.0)) == bool4(true, false, false, true)));
}
