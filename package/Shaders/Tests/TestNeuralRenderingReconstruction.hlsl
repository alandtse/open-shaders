#include "/Shaders/Common/NeuralRenderingReconstruction.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

/// @tags neural-rendering
/// RGB guidance preserves positive normalized weights and constant changes
[numthreads(1, 1, 1)] void TestRgbGuidanceHasPositiveUnitWeights() {
	float4 weights;
	ASSERT(IsTrue, NeuralRenderingReconstruction::RgbWeights(0.5, 0.2, 0.5, 0.8, 0.9, 0.25, kLuma, weights));
	ASSERT(IsTrue, all(weights >= 0.0));
	ASSERT(IsTrue, abs(dot(weights, 1.0) - 1.0) < 1e-6);
	ASSERT(IsTrue, abs(dot(weights, 0.3) - 0.3) < 1e-6);
}

	/// @tags neural-rendering
	/// RGB guidance distinguishes equally bright colours across an edge
	[numthreads(1, 1, 1)] void TestRgbGuidancePreservesChromaticEdges()
{
	const float3 red = float3(1.0, 0.0, 0.0);
	const float3 green = float3(0.0, kLuma.r / kLuma.g, 0.0);
	float4 weights;
	ASSERT(IsTrue, abs(dot(red - green, kLuma)) < 1e-6);
	ASSERT(IsTrue, NeuralRenderingReconstruction::RgbWeights(red, red, green, red, green, 0.25, kLuma, weights));
	ASSERT(IsTrue, weights.x > weights.y);
	ASSERT(IsTrue, weights.z > weights.w);
}

/// @tags neural-rendering
/// A common exposure change does not move the guided weights
[numthreads(1, 1, 1)] void TestRgbGuidanceIsExposureInvariant() {
	const float3 reference = float3(0.2, 0.5, 0.3);
	const float3 a = float3(0.4, 0.1, 0.2), b = float3(0.1, 0.2, 0.8);
	float4 base, scaled;
	ASSERT(IsTrue, NeuralRenderingReconstruction::RgbWeights(reference, a, b, reference, a, 0.25, kLuma, base));
	ASSERT(IsTrue, NeuralRenderingReconstruction::RgbWeights(reference * 1000.0, a * 1000.0, b * 1000.0, reference * 1000.0, a * 1000.0, 0.25, kLuma, scaled));
	ASSERT(IsTrue, all(abs(base - scaled) < 1e-5));
}

	/// @tags neural-rendering
	/// Bad optional guidance leaves the valid spatial weights intact
	[numthreads(1, 1, 1)] void TestRgbGuidanceFallsBackToSpatialWeights()
{
	const float4 spatial = float4(0.12, 0.28, 0.18, 0.42);
	float4 weights;
	ASSERT(IsTrue, !NeuralRenderingReconstruction::RgbWeights(asfloat(0x7FC00000u), 0.2, 0.3, 0.4, 0.5, spatial, kLuma, weights));
	ASSERT(IsTrue, all(weights == spatial));
	ASSERT(IsTrue, !NeuralRenderingReconstruction::RgbWeights(0.2, asfloat(0x7F800000u), 0.3, 0.4, 0.5, spatial, kLuma, weights));
	ASSERT(IsTrue, all(weights == spatial));
	ASSERT(IsTrue, NeuralRenderingReconstruction::RgbWeights(0.0, 0.0, 0.0, 0.0, 0.0, spatial, kLuma, weights));
	ASSERT(IsTrue, all(weights == spatial));
}
