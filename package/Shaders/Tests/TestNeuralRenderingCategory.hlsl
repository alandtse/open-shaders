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

/// @tags neural-rendering
/// The material map filter enables exactly the categories whose bit is set
[numthreads(1, 1, 1)] void TestCategoryInFilterSelectsBits() {
	const uint skinAndEyes = (1u << NeuralRenderingCategory::Skin) | (1u << NeuralRenderingCategory::Eyes);
	ASSERT(IsTrue, NeuralRenderingCategory::CategoryInFilter(NeuralRenderingCategory::Skin, skinAndEyes));
	ASSERT(IsTrue, NeuralRenderingCategory::CategoryInFilter(NeuralRenderingCategory::Eyes, skinAndEyes));
	ASSERT(IsTrue, !NeuralRenderingCategory::CategoryInFilter(NeuralRenderingCategory::Hair, skinAndEyes));
	ASSERT(IsTrue, !NeuralRenderingCategory::CategoryInFilter(NeuralRenderingCategory::None, skinAndEyes));
	ASSERT(IsTrue, NeuralRenderingCategory::CategoryInFilter(NeuralRenderingCategory::Landscape, 0x3Fu));
	ASSERT(IsTrue, !NeuralRenderingCategory::CategoryInFilter(NeuralRenderingCategory::None, 0u));
}

	/// @tags neural-rendering
	/// CategoryStrength reads the strength of one category out of the packed float4 + float2 pair
	[numthreads(1, 1, 1)] void TestCategoryStrengthSelectsTheCategory()
{
	const float4 low = float4(0.125, 0.25, 0.375, 0.5);
	const float2 high = float2(0.625, 0.75);
	ASSERT(AreEqual, NeuralRenderingCategory::CategoryStrength(NeuralRenderingCategory::None, low, high), 0.125);
	ASSERT(AreEqual, NeuralRenderingCategory::CategoryStrength(NeuralRenderingCategory::Skin, low, high), 0.25);
	ASSERT(AreEqual, NeuralRenderingCategory::CategoryStrength(NeuralRenderingCategory::Hair, low, high), 0.375);
	ASSERT(AreEqual, NeuralRenderingCategory::CategoryStrength(NeuralRenderingCategory::Eyes, low, high), 0.5);
	ASSERT(AreEqual, NeuralRenderingCategory::CategoryStrength(NeuralRenderingCategory::Foliage, low, high), 0.625);
	ASSERT(AreEqual, NeuralRenderingCategory::CategoryStrength(NeuralRenderingCategory::Landscape, low, high), 0.75);
	ASSERT(AreEqual, NeuralRenderingCategory::CategoryStrength(200u, low, high), 0.125);
}

/// @tags neural-rendering
/// DebugColor keeps the colours the Neural Rendering settings legend draws for each category
[numthreads(1, 1, 1)] void TestDebugColorMatchesTheLegend() {
	ASSERT(IsTrue, all(NeuralRenderingCategory::DebugColor(NeuralRenderingCategory::None) == float3(0.05, 0.05, 0.05)));
	ASSERT(IsTrue, all(NeuralRenderingCategory::DebugColor(NeuralRenderingCategory::Skin) == float3(1.0, 0.0, 0.0)));
	ASSERT(IsTrue, all(NeuralRenderingCategory::DebugColor(NeuralRenderingCategory::Hair) == float3(1.0, 0.5, 0.0)));
	ASSERT(IsTrue, all(NeuralRenderingCategory::DebugColor(NeuralRenderingCategory::Eyes) == float3(1.0, 1.0, 0.0)));
	ASSERT(IsTrue, all(NeuralRenderingCategory::DebugColor(NeuralRenderingCategory::Foliage) == float3(0.0, 1.0, 0.0)));
	ASSERT(IsTrue, all(NeuralRenderingCategory::DebugColor(NeuralRenderingCategory::Landscape) == float3(0.0, 1.0, 1.0)));
}
