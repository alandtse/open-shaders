// HLSL Unit Tests for Common/UIComposition.hlsli
#include "/Shaders/Common/UIComposition.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags utility, uicomposition
[numthreads(1, 1, 1)] void TestUICompositionPremultipliedCoverageIsItsAlpha() {
	ASSERT(AreEqual, UIComposition::EffectiveCoverage(float4(0.25, 0.5, 0.0, 0.5)), 0.5);
	ASSERT(AreEqual, UIComposition::EffectiveCoverage(float4(0.0, 0.0, 0.0, 0.75)), 0.75);
}

	/// @tags utility, uicomposition
	[numthreads(1, 1, 1)] void TestUICompositionAdditiveColourCountsAsCoverage()
{
	ASSERT(AreEqual, UIComposition::EffectiveCoverage(float4(0.5, 0.25, 0.0, 0.0)), 0.5);
	ASSERT(AreEqual, UIComposition::EffectiveCoverage(float4(0.0, 0.0, 0.75, 0.25)), 0.75);
	ASSERT(AreEqual, UIComposition::EffectiveCoverage(float4(0.0, 0.0, 0.0, 0.0)), 0.0);
}
