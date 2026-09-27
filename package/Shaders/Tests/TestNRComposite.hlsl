// HLSL unit tests for the NR composite gain (Upscaling/NeuralRendering/ColorContract.hlsli).
//
// The pin keeps Common/Color.hlsli from aliasing ENABLE_LL to the bound cbuffer member, which
// DXC's lib_6_x unit-test target cannot resolve; VSHADER is the guard that suppresses the alias.
#define ENABLE_LL 1
#define VSHADER

#include "/Shaders/Common/Color.hlsli"
#include "/Shaders/Upscaling/NeuralRendering/ColorContract.hlsli"

#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags nr, color
[numthreads(1, 1, 1)] void TestNRCompositeGainIsAnExactPassthroughAtZeroTone() {
	ASSERT(AreEqual, NR::CompositeGain(0.0f, NR::kMaxToneStops), 1.0f);
	ASSERT(AreEqual, NR::CompositeGain(0.0f, 0.0f), 1.0f);
}

	/// @tags nr, color
	[numthreads(1, 1, 1)] void TestNRCompositeGainPassesANonFiniteToneThrough()
{
	const float notANumber = asfloat(0x7FC00000u);
	const float positiveInfinity = asfloat(0x7F800000u);
	const float negativeInfinity = -positiveInfinity;

	ASSERT(AreEqual, NR::CompositeGain(notANumber, NR::kMaxToneStops), 1.0f);
	ASSERT(AreEqual, NR::CompositeGain(positiveInfinity, NR::kMaxToneStops), 1.0f);
	ASSERT(AreEqual, NR::CompositeGain(negativeInfinity, NR::kMaxToneStops), 1.0f);
}

/// @tags nr, color
[numthreads(1, 1, 1)] void TestNRCompositeGainStaysWithinItsToneStops() {
	const float largest = 3.402823466e+38f;
	float tones[6] = { 1e4f, -1e4f, largest, -largest, NR::kMaxToneStops, -NR::kMaxToneStops };
	for (int i = 0; i < 6; ++i) {
		float gain = NR::CompositeGain(tones[i], NR::kMaxToneStops);
		ASSERT(IsTrue, isfinite(gain));
		ASSERT(IsTrue, gain > 0.0f);
		ASSERT(IsTrue, abs(log2(gain)) <= NR::kMaxToneStops + 1e-4f);
	}

	// The clamp saturates instead of growing with the input.
	ASSERT(IsTrue, abs(NR::CompositeGain(1e4f, NR::kMaxToneStops) - exp2(NR::kMaxToneStops)) < 1e-3f);
	ASSERT(IsTrue, abs(NR::CompositeGain(-1e4f, NR::kMaxToneStops) - exp2(-NR::kMaxToneStops)) < 1e-3f);
}
