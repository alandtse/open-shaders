// HLSL Unit Tests for Common/FoveatedShaderDetail.hlsli
// Covers the cross-eye detail-weight combine used by the VR SSR foveation path.
#include "/Shaders/Common/FoveatedShaderDetail.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags foveated, vr
/// The lower of the two eye weights wins, whichever eye it belongs to
[numthreads(1, 1, 1)] void TestFoveatedCombineKeepsLowerWeight() {
	ASSERT(AreEqual, FoveatedCombineEyeWeights(1.0, 0.25, true), 0.25);
	ASSERT(AreEqual, FoveatedCombineEyeWeights(0.25, 1.0, true), 0.25);
}

	/// @tags foveated, vr
	/// An invalid reprojection keeps this eye's own weight, ignoring the other eye's
	[numthreads(1, 1, 1)] void TestFoveatedCombineInvalidKeepsOwnWeight()
{
	ASSERT(AreEqual, FoveatedCombineEyeWeights(0.6, 0.1, false), 0.6);
	ASSERT(AreEqual, FoveatedCombineEyeWeights(0.3, 1.0, false), 0.3);
	ASSERT(AreEqual, FoveatedCombineEyeWeights(0.3, 0.0, false), 0.3);
}

/// @tags foveated, vr
/// Equal weights stay unchanged
[numthreads(1, 1, 1)] void TestFoveatedCombineEqualWeights() {
	ASSERT(AreEqual, FoveatedCombineEyeWeights(0.5, 0.5, true), 0.5);
}

	/// @tags foveated, vr
	/// A zero weight in either eye drives the combined weight to zero
	[numthreads(1, 1, 1)] void TestFoveatedCombineZeroWeight()
{
	ASSERT(AreEqual, FoveatedCombineEyeWeights(1.0, 0.0, true), 0.0);
	ASSERT(AreEqual, FoveatedCombineEyeWeights(0.0, 1.0, true), 0.0);
}

/// @tags foveated, vr
/// Hard-cutoff weights combine to the same 0/1 values
[numthreads(1, 1, 1)] void TestFoveatedCombineHardCutoff() {
	ASSERT(AreEqual, FoveatedCombineEyeWeights(1.0, 1.0, true), 1.0);
	ASSERT(AreEqual, FoveatedCombineEyeWeights(1.0, 0.0, true), 0.0);
	ASSERT(AreEqual, FoveatedCombineEyeWeights(0.0, 1.0, true), 0.0);
}
