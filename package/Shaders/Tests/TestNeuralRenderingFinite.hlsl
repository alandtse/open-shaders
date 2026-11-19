#include "/Shaders/Common/NeuralRenderingFinite.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

static const uint kSignBit = 1u << 31u;

/// @tags neural-rendering
/// Signed zero, subnormals and finite extremes remain valid
[numthreads(1, 1, 1)] void TestFiniteIncludesFloatingPointBoundaries() {
	ASSERT(IsTrue, NeuralRenderingFinite::IsFinite(0.0));
	ASSERT(IsTrue, NeuralRenderingFinite::IsFinite(asfloat(kSignBit)));
	ASSERT(IsTrue, all(NeuralRenderingFinite::IsFinite(asfloat(uint4(0x00000001u, kSignBit | 0x00000001u, 0x007FFFFFu, 0x00800000u)))));
	ASSERT(IsTrue, all(NeuralRenderingFinite::IsFinite(asfloat(uint2(0x7F7FFFFFu, kSignBit | 0x7F7FFFFFu)))));
}

	/// @tags neural-rendering
	/// Both infinities and signed quiet or signalling NaNs are rejected
	[numthreads(1, 1, 1)] void TestFiniteRejectsInfinityAndNan()
{
	ASSERT(IsTrue, !NeuralRenderingFinite::IsFinite(asfloat(0x7F800000u)));
	ASSERT(IsTrue, !NeuralRenderingFinite::IsFinite(asfloat(kSignBit | 0x7F800000u)));
	ASSERT(IsTrue, all(!NeuralRenderingFinite::IsFinite(asfloat(uint4(0x7FC00000u, kSignBit | 0x7FC00000u, 0x7F800001u, kSignBit | 0x7F800001u)))));
}

/// @tags neural-rendering
/// Mixed vectors retain per-component validity
[numthreads(1, 1, 1)] void TestFiniteClassifiesVectorComponents() {
	ASSERT(IsTrue, all(NeuralRenderingFinite::IsFinite(asfloat(uint2(0x7FC00000u, kSignBit | 0x3F800000u))) == bool2(false, true)));
	ASSERT(IsTrue, all(NeuralRenderingFinite::IsFinite(asfloat(uint3(0x3F800000u, 0x7F800000u, 0x7FC00000u))) == bool3(true, false, false)));
	ASSERT(IsTrue, all(NeuralRenderingFinite::IsFinite(asfloat(uint4(0x00000000u, kSignBit | 0x7F800000u, kSignBit, 0x7F7FFFFFu))) == bool4(true, false, true, true)));
}

	/// @tags neural-rendering
	/// Accepted RGB remains representable in half storage without changing source alpha
	[numthreads(1, 1, 1)] void TestHalfOutputBoundsFiniteRgb()
{
	const float4 source = float4(0.1, 0.2, 0.3, 0.375);
	ASSERT(IsTrue, all(NeuralRenderingFinite::ResolveHalfOutput(source, float3(-2.0, 0.0, 4.0), true) == float4(-2.0, 0.0, 4.0, source.a)));
	ASSERT(IsTrue, all(NeuralRenderingFinite::ResolveHalfOutput(source, float3(-1e20, 0.5, 1e20), true) == float4(-65504.0, 0.5, 65504.0, source.a)));
	ASSERT(IsTrue, all(NeuralRenderingFinite::ResolveHalfOutput(source, float3(-65504.0, 1.0, 65504.0), true) == float4(-65504.0, 1.0, 65504.0, source.a)));
}

/// @tags neural-rendering
/// Rejected model samples or non-finite candidates retain the source pixel
[numthreads(1, 1, 1)] void TestHalfOutputRejectsInvalidReconstruction() {
	const float4 source = float4(0.1, 0.2, 0.3, 0.375);
	ASSERT(IsTrue, all(NeuralRenderingFinite::ResolveHalfOutput(source, 0.5, false) == source));
	ASSERT(IsTrue, all(NeuralRenderingFinite::ResolveHalfOutput(source, float3(asfloat(0x7FC00000u), 0.5, 0.5), true) == source));
	ASSERT(IsTrue, all(NeuralRenderingFinite::ResolveHalfOutput(source, float3(0.5, asfloat(0x7F800000u), 0.5), true) == source));
	ASSERT(IsTrue, all(NeuralRenderingFinite::ResolveHalfOutput(source, float3(0.5, 0.5, asfloat(kSignBit | 0x7F800000u)), true) == source));
}

	/// @tags neural-rendering
	/// A corrupt source is preserved rather than replaced with a reconstruction
	[numthreads(1, 1, 1)] void TestHalfOutputPreservesInvalidSource()
{
	const float4 source = asfloat(uint4(0x7FC00000u, 0x3F800000u, 0x7F800000u, 0x3F000000u));
	ASSERT(IsTrue, all(asuint(NeuralRenderingFinite::ResolveHalfOutput(source, 0.5, true)) == asuint(source)));
	const float4 invalidAlpha = float4(0.1, 0.2, 0.3, asfloat(0x7F800000u));
	ASSERT(IsTrue, all(asuint(NeuralRenderingFinite::ResolveHalfOutput(invalidAlpha, 0.5, true)) == asuint(invalidAlpha)));
}
