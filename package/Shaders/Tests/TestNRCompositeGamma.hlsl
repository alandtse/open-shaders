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

/// @tags nr, color
/// The final chroma gain keeps linear luminance and the stop bound after the gamma re-encode
[numthreads(1, 1, 1)] void TestNRCompositeGammaChromaGainKeepsLuminanceAndBound() {
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float3 linearBase = float3(0.2f, 0.0f, 0.25f);
	const float3 applied = NR::CompositeChromaGain(Color::LinearToSkyrimGamma(linearBase), float3(-2.0f, 0.3925f, 2.0f), 1.0f, luma, NR::kMaxChromaStops);
	const float3 gain = Color::SkyrimGammaToLinear(applied);
	ASSERT(IsTrue, abs(dot(luma, linearBase * gain) - dot(luma, linearBase)) < 1e-3f);
	ASSERT(IsTrue, all(gain <= 2.0f + 1e-3f) && all(gain >= 0.5f - 1e-3f));
}
