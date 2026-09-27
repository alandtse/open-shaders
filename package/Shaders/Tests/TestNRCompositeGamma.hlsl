// VSHADER suppresses Color.hlsli's alias of ENABLE_LL to a cbuffer member, which DXC's
// lib_6_x unit-test target cannot resolve. ENABLE_LL 0 covers the gamma output domain.
#define ENABLE_LL 0
#define VSHADER

#include "/Shaders/Common/Color.hlsli"
#include "/Shaders/Upscaling/NeuralRendering/ColorContract.hlsli"

#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags nr, color
[numthreads(1, 1, 1)] void TestNRCompositeGammaReEncodesTheBoundedGain() {
	ASSERT(AreEqual, NR::CompositeGain(0.0f, NR::kMaxToneStops), 1.0f);

	const float bounded = NR::CompositeGain(1e4f, NR::kMaxToneStops);
	ASSERT(AreEqual, bounded, Color::LinearToSkyrimGamma(exp2(NR::kMaxToneStops)));
	ASSERT(IsTrue, abs(bounded - exp2(NR::kMaxToneStops)) > 1e-3f);
}

	/// @tags nr, color
	[numthreads(1, 1, 1)] void TestNRCompositeGammaPassesANonFiniteToneThrough()
{
	ASSERT(AreEqual, NR::CompositeGain(asfloat(0x7FC00000u), NR::kMaxToneStops), 1.0f);
	ASSERT(AreEqual, NR::CompositeGain(asfloat(0x7F800000u), NR::kMaxToneStops), 1.0f);
	ASSERT(AreEqual, NR::CompositeGain(-asfloat(0x7F800000u), NR::kMaxToneStops), 1.0f);
}
