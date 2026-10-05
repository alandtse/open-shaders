// HLSL Unit Tests for Common/NeuralRenderingCategory.hlsli
// Covers the Masks2 material category encoding that survives alpha blending.
#include "/Shaders/Common/NeuralRenderingCategory.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags neural-rendering
/// Every category id decodes back to itself after encoding
[numthreads(1, 1, 1)] void TestCategoryRoundTrip() {
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(NeuralRenderingCategory::None)), NeuralRenderingCategory::None);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Skin)), NeuralRenderingCategory::Skin);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Hair)), NeuralRenderingCategory::Hair);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Eyes)), NeuralRenderingCategory::Eyes);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Foliage)), NeuralRenderingCategory::Foliage);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Landscape)), NeuralRenderingCategory::Landscape);
}

	/// @tags neural-rendering
	/// An id outside the range encodes as None
	[numthreads(1, 1, 1)] void TestCategoryOutOfRangeEncodesNone()
{
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Count)), NeuralRenderingCategory::None);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(NeuralRenderingCategory::Encode(200u)), NeuralRenderingCategory::None);
}

/// @tags neural-rendering
/// An even blend of two different categories never decodes as a third material
[numthreads(1, 1, 1)] void TestCategoryEvenBlendDecodesNone() {
	const float hairOverNone = lerp(NeuralRenderingCategory::Encode(NeuralRenderingCategory::None), NeuralRenderingCategory::Encode(NeuralRenderingCategory::Hair), 0.5);
	const float hairOverSkin = lerp(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Skin), NeuralRenderingCategory::Encode(NeuralRenderingCategory::Hair), 0.5);
	const float foliageOverLandscape = lerp(NeuralRenderingCategory::Encode(NeuralRenderingCategory::Foliage), NeuralRenderingCategory::Encode(NeuralRenderingCategory::Landscape), 0.5);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(hairOverNone), NeuralRenderingCategory::None);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(hairOverSkin), NeuralRenderingCategory::None);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(foliageOverLandscape), NeuralRenderingCategory::None);
}

	/// @tags neural-rendering
	/// A cleared or slightly off value decodes as None unless it sits on a code
	[numthreads(1, 1, 1)] void TestCategoryOffCodeDecodesNone()
{
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(0.0), NeuralRenderingCategory::None);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(26.0 / 255.0), NeuralRenderingCategory::None);
	ASSERT(AreEqual, NeuralRenderingCategory::Decode(1.0), NeuralRenderingCategory::None);
}
