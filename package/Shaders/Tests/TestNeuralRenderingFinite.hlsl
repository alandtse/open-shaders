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
