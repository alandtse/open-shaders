#include "/Shaders/Common/ColorSpaces.hlsli"
#include "/Shaders/PostProcessing/ColorGrading/Include/Highlights.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

namespace TestConstants
{
	static const float TOLERANCE = 0.00001f;
	static const uint RAMP_STEPS = 1024;
}

/// @tags color, highlights, hdr, regression
[numthreads(1, 1, 1)] void TestHighlightCompressionPreservesHDRHeadroom() {
	float3 tint = float3(1.5f, 1.0f, 0.25f);
	tint /= dot(tint, Rec2020_2_XYZ_MAT[1]);
	float3 previous = 0.0f;
	for (uint i = 0; i <= TestConstants::RAMP_STEPS; ++i) {
		float luma = exp2(12.0f * i / TestConstants::RAMP_STEPS);
		float3 color = tint * luma;
		float3 graded = Highlights::Apply(color, luma, 1.0f, 0.446f, 0.0f, -0.5f, 0.55f, 1.0f);
		ASSERT(IsTrue, all(isfinite(graded)));
		ASSERT(IsTrue, all(graded + TestConstants::TOLERANCE >= previous));
		ASSERT(IsTrue, all(graded <= color + TestConstants::TOLERANCE));
		previous = graded;
	}
	ASSERT(IsTrue, all(previous > 1.0f));
}

	/// @tags color, highlights, regression
	[numthreads(1, 1, 1)] void TestReducedHighlightsPreserveBrightnessOrder()
{
	float3 highlightsGain = float3(0.0f, 0.446f, 0.9f);
	float3 previous = 0.0f;
	for (uint i = 1; i <= TestConstants::RAMP_STEPS; ++i) {
		float luma = 8.0f * i / TestConstants::RAMP_STEPS;
		float3 graded = Highlights::Apply(luma, luma, 1.0f, highlightsGain, 0.0f, 0.0f, 0.55f, 1.0f);
		ASSERT(IsTrue, all(graded + TestConstants::TOLERANCE >= previous));
		ASSERT(IsTrue, all(graded <= luma + TestConstants::TOLERANCE));
		previous = graded;
	}
}

/// @tags color, highlights, regression
[numthreads(1, 1, 1)] void TestHighlightGainPreservesNeutralAndBoostedControls() {
	for (uint i = 0; i <= TestConstants::RAMP_STEPS; ++i) {
		float luma = 4.0f * i / TestConstants::RAMP_STEPS;
		float3 neutral = Highlights::Apply(luma, luma, 1.0f, 1.0f, 0.0f, 0.0f, 0.55f, 1.0f);
		float3 boosted = Highlights::Apply(luma, luma, 1.0f, float3(1.1f, 1.5f, 2.0f), 0.0f, 0.0f, 0.55f, 1.0f);
		float3 expected = luma * lerp(1.0f, float3(1.1f, 1.5f, 2.0f), smoothstep(0.55f, 1.0f, luma));
		ASSERT(IsTrue, all(neutral == luma));
		ASSERT(IsTrue, all(abs(boosted - expected) < TestConstants::TOLERANCE));
	}
}

	/// @tags color, highlights, regression
	[numthreads(1, 1, 1)] void TestHighlightGainPreservesMidtonesAndHDRSlope()
{
	float3 midtonesGain = float3(0.5f, 1.0f, 1.5f);
	float3 highlightsGain = float3(0.1f, 0.446f, 0.9f);

	ASSERT(IsTrue, all(Highlights::Apply(0.0f, 0.0f, midtonesGain, highlightsGain, 0.0f, 0.0f, 0.55f, 1.0f) == 0.0f));
	ASSERT(IsTrue, all(Highlights::Apply(0.55f, 0.55f, midtonesGain, highlightsGain, 0.0f, 0.0f, 0.55f, 1.0f) == 0.55f * midtonesGain));
	float3 lower = Highlights::Apply(4.0f, 4.0f, midtonesGain, highlightsGain, 0.0f, 0.0f, 0.55f, 1.0f);
	float3 upper = Highlights::Apply(8.0f, 8.0f, midtonesGain, highlightsGain, 0.0f, 0.0f, 0.55f, 1.0f);
	ASSERT(IsTrue, all(abs((upper - lower) / 4.0f - highlightsGain) < TestConstants::TOLERANCE));
}

/// @tags color, highlights, robustness
[numthreads(1, 1, 1)] void TestHighlightGainHandlesNarrowRanges() {
	float3 previous = 0.0f;
	for (uint i = 0; i <= TestConstants::RAMP_STEPS; ++i) {
		float luma = 2.0f * i / TestConstants::RAMP_STEPS;
		float3 graded = Highlights::Apply(luma, luma, 1.0f, 0.446f, 0.0f, -0.5f, 0.55f, 0.55f);
		ASSERT(IsTrue, all(isfinite(graded)));
		ASSERT(IsTrue, all(graded + TestConstants::TOLERANCE >= previous));
		previous = graded;
	}
}

	/// @tags color, highlights, regression
	[numthreads(1, 1, 1)] void TestNegativeOffsetsPreserveBrightnessOrderWithGain()
{
	float3 highlightsGain = float3(0.0f, 0.446f, 2.0f);
	float3 tint = float3(1.5f, 1.0f, 0.25f);
	float3 midtonesOffset = float3(0.5f, 0.0f, -0.25f);
	float3 previous = midtonesOffset;
	for (uint i = 1; i <= TestConstants::RAMP_STEPS; ++i) {
		float luma = 16.0f * i / TestConstants::RAMP_STEPS;
		float3 graded = Highlights::Apply(tint * luma, luma, 1.0f, highlightsGain, midtonesOffset, -0.5f, 0.55f, 1.0f);
		float3 withoutOffset = Highlights::Apply(tint * luma, luma, 1.0f, highlightsGain, midtonesOffset, midtonesOffset, 0.55f, 1.0f);
		ASSERT(IsTrue, all(isfinite(graded)));
		ASSERT(IsTrue, all(graded + TestConstants::TOLERANCE >= previous));
		ASSERT(IsTrue, all(graded <= withoutOffset + TestConstants::TOLERANCE));
		ASSERT(IsTrue, all(graded >= withoutOffset - (midtonesOffset + 0.5f) - TestConstants::TOLERANCE));
		previous = graded;
	}
}

/// @tags color, highlights, regression
[numthreads(1, 1, 1)] void TestHighlightOffsetsPreserveMidtonesAndPositiveControls() {
	float3 offset = float3(0.1f, 0.25f, 0.5f);
	ASSERT(IsTrue, all(Highlights::Apply(0.55f, 0.55f, 1.0f, 0.446f, 0.0f, -0.5f, 0.55f, 1.0f) == 0.55f));
	for (uint i = 0; i <= TestConstants::RAMP_STEPS; ++i) {
		float luma = 4.0f * i / TestConstants::RAMP_STEPS;
		float3 graded = Highlights::Apply(luma, luma, 1.0f, 1.0f, 0.0f, offset, 0.55f, 1.0f);
		float3 expected = luma + offset * smoothstep(0.55f, 1.0f, luma);
		ASSERT(IsTrue, all(abs(graded - expected) < TestConstants::TOLERANCE));
	}
	float3 hdr = Highlights::Apply(1024.0f, 1024.0f, 1.0f, 1.0f, 0.0f, -0.5f, 0.55f, 1.0f);
	ASSERT(IsTrue, all(abs(hdr - 1023.5f) < 0.001f));
}
