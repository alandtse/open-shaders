// HLSL Unit Tests for Common/FeatherBlend.hlsli
#include "/Shaders/Common/FeatherBlend.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags utility, featherblend
[numthreads(1, 1, 1)] void TestFeatherBlendShapeRampClampsAndShapes() {
	ASSERT(AreEqual, FeatherBlend::ShapeRamp(-1.0f, 1.0f), 0.0f);
	ASSERT(AreEqual, FeatherBlend::ShapeRamp(2.0f, 1.0f), 1.0f);
	ASSERT(AreEqual, FeatherBlend::ShapeRamp(0.5f, 1.0f), 0.5f);
	ASSERT(AreEqual, FeatherBlend::ShapeRamp(0.5f, 2.0f), 0.25f);
}

	/// @tags utility, featherblend
	[numthreads(1, 1, 1)] void TestFeatherBlendSmoothRampIsFlatAtBothEnds()
{
	ASSERT(AreEqual, FeatherBlend::SmoothRamp(0.0f), 0.0f);
	ASSERT(AreEqual, FeatherBlend::SmoothRamp(1.0f), 1.0f);
	ASSERT(AreEqual, FeatherBlend::SmoothRamp(0.5f), 0.5f);
	ASSERT(IsTrue, FeatherBlend::SmoothRamp(0.1f) < 0.1f);
	ASSERT(IsTrue, FeatherBlend::SmoothRamp(0.9f) > 0.9f);
}
