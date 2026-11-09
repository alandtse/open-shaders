// VSHADER suppresses Color.hlsli's alias of ENABLE_LL to a cbuffer member, which DXC's
// lib_6_x unit-test target cannot resolve.
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

	/// @tags nr, color
	/// A zero strength and an unchanged model both leave the chroma gain at exactly one
	[numthreads(1, 1, 1)] void TestNRChromaGainIsAPassthroughAtZeroStrengthOrNoChange()
{
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float3 input = float3(0.18f, 0.32f, 0.55f);
	const float3 shifted = float3(0.30f, 0.30f, 0.20f);
	const float3 same = NR::ChromaGain(NR::ChromaOffset(input, input, luma, NR::kChromaFloor), 1.0f, NR::kMaxChromaStops);
	const float3 off = NR::ChromaGain(NR::ChromaOffset(input, shifted, luma, NR::kChromaFloor), 0.0f, NR::kMaxChromaStops);
	ASSERT(IsTrue, all(abs(same - 1.0f) < 1e-6f));
	ASSERT(IsTrue, all(abs(off - 1.0f) < 1e-6f));
}

/// @tags nr, color
/// The chroma taper is 1 well below the proxy's clip and 0 at it
[numthreads(1, 1, 1)] void TestNRChromaHeadroomFadesAtClip() {
	ASSERT(IsTrue, abs(NR::ChromaHeadroom(float3(0.2f, 0.5f, 0.8f)) - 1.0f) < 1e-6f);
	ASSERT(IsTrue, abs(NR::ChromaHeadroom(float3(0.2f, 0.5f, 1.0f))) < 1e-6f);
	const float mid = NR::ChromaHeadroom(float3(0.2f, 0.5f, 0.92f));
	ASSERT(IsTrue, mid > 0.0f && mid < 1.0f);
}

	/// @tags nr, color
	/// The final applied chroma gain keeps linear luminance and the stop bound for a coloured input, in the compiled colour domain
	[numthreads(1, 1, 1)] void TestNRCompositeChromaGainKeepsLuminanceAndBound()
{
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float3 linearBase = float3(0.2f, 0.0f, 0.25f);
	const float3 gameColor = ENABLE_LL ? linearBase : Color::LinearToSkyrimGamma(linearBase);
	const float3 applied = NR::CompositeChromaGain(gameColor, float3(-2.0f, 0.3925f, 2.0f), 1.0f, luma, NR::kMaxChromaStops);
	const float3 gain = ENABLE_LL ? applied : Color::SkyrimGammaToLinear(applied);
	ASSERT(IsTrue, abs(dot(luma, linearBase * gain) - dot(luma, linearBase)) < 1e-4f);
	ASSERT(IsTrue, all(gain <= 2.0f + 1e-5f) && all(gain >= 0.5f - 1e-5f));
}

/// @tags nr, color
/// Applied to a neutral input, a zero-mean offset still shifts luminance; the preserving gain removes it
[numthreads(1, 1, 1)] void TestNRLuminancePreservingGainKeepsLuminance() {
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float3 input = float3(0.5f, 0.5f, 0.5f);
	const float3 gain = exp2(float3(1.0f, -0.297f, 0.0f));
	ASSERT(IsTrue, abs(dot(luma, input * gain) - dot(luma, input)) > 0.02f);
	const float3 kept = NR::LuminancePreservingGain(input, gain, luma, NR::kMaxChromaStops);
	ASSERT(IsTrue, abs(dot(luma, input * kept) - dot(luma, input)) < 1e-5f);
	ASSERT(IsTrue, all(NR::LuminancePreservingGain(float3(0, 0, 0), gain, luma, NR::kMaxChromaStops) == gain));
	const float3 bounded = NR::LuminancePreservingGain(input, float3(2.0f, exp2(-0.5945f), 1.0f), luma, NR::kMaxChromaStops);
	ASSERT(IsTrue, all(bounded <= 2.0f + 1e-5f) && all(bounded >= 0.5f - 1e-5f));
	ASSERT(IsTrue, abs(dot(luma, input * bounded) - dot(luma, input)) < 2e-3f);
}

	/// @tags nr, color
	/// The offset carries colour only: its luminance-weighted mean is zero, and a pure brightness change gives none
	[numthreads(1, 1, 1)] void TestNRChromaOffsetIsLuminanceNeutral()
{
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float3 input = float3(0.18f, 0.32f, 0.55f);
	const float3 offset = NR::ChromaOffset(input, float3(0.40f, 0.25f, 0.20f), luma, NR::kChromaFloor);
	ASSERT(IsTrue, abs(dot(luma, offset)) < 1e-5f);
	ASSERT(IsTrue, any(abs(offset) > 0.05f));
	const float3 brighter = NR::ChromaOffset(input, input * 2.0f, luma, 0.0f);
	ASSERT(IsTrue, all(abs(brighter) < 1e-5f));
}

/// @tags nr, color
/// Storing the red and blue offsets reproduces the full offset
[numthreads(1, 1, 1)] void TestNRChromaRedBlueRoundTrip() {
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float3 offset = NR::ChromaOffset(float3(0.18f, 0.32f, 0.55f), float3(0.40f, 0.25f, 0.20f), luma, NR::kChromaFloor);
	const float3 restored = NR::ChromaFromRedBlue(float2(offset.x, offset.z), luma);
	ASSERT(IsTrue, all(abs(restored - offset) < 1e-5f));
}

	/// @tags nr, color
	/// A runaway or non-finite offset stays inside the chroma cap or is ignored
	[numthreads(1, 1, 1)] void TestNRChromaGainIsBounded()
{
	const float3 huge = float3(1e4f, -1e4f, 0.0f);
	const float3 gain = NR::ChromaGain(huge, 1.0f, NR::kMaxChromaStops);
	ASSERT(IsTrue, all(isfinite(gain)));
	ASSERT(IsTrue, all(abs(log2(gain)) <= NR::kMaxChromaStops + 1e-4f));
	const float3 notANumber = float3(asfloat(0x7FC00000u), 0.0f, 0.0f);
	ASSERT(IsTrue, all(abs(NR::ChromaGain(notANumber, 1.0f, NR::kMaxChromaStops) - 1.0f) < 1e-6f));
}
