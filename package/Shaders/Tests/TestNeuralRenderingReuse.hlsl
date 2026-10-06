// HLSL Unit Tests for Common/NeuralRenderingReuse.hlsli
// Covers the eye-stagger's reprojection validity test and the gain it applies to the current proxy.
#include "/Shaders/Common/NeuralRenderingReuse.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

// SharedData's projection terms for a 5 to 120000 unit view: x far, y near, z far - near, w far * near.
static const float4 kCameraData = float4(120000.0f, 5.0f, 119995.0f, 600000.0f);

/// @brief Skyrim's raw depth for a view-space distance, the inverse of LinearDepth; near is 0, far is 1.
float RawDepthForDistance(float distance, float4 cameraData)
{
	return (cameraData.x - cameraData.w / distance) / cameraData.z;
}

/// @tags neural-rendering
/// LinearDepth reads a raw guide depth back as the view-space distance the engine compares
[numthreads(1, 1, 1)] void TestReuseLinearDepthInvertsTheRawDepth() {
	ASSERT(IsTrue, abs(NeuralRenderingReuse::LinearDepth(0.0f, kCameraData, false) - kCameraData.y) < 0.01f);
	ASSERT(IsTrue, abs(NeuralRenderingReuse::LinearDepth(1.0f, kCameraData, true) - kCameraData.y) < 0.01f);
	ASSERT(IsTrue, abs(NeuralRenderingReuse::LinearDepth(RawDepthForDistance(250.0f, kCameraData), kCameraData, false) - 250.0f) < 0.05f);
	// A reverse-Z guide holds the mirrored depth, and the mirrored branch must read it back the same.
	ASSERT(IsTrue, abs(NeuralRenderingReuse::LinearDepth(1.0f - RawDepthForDistance(250.0f, kCameraData), kCameraData, true) - 250.0f) < 0.05f);
}

	/// @tags neural-rendering
	/// A reprojected sample under the same surface and lighting is reused, in either depth convention
	[numthreads(1, 1, 1)] void TestReuseAcceptsAMatchedSample()
{
	const float depth = RawDepthForDistance(250.0f, kCameraData);
	ASSERT(IsTrue, NeuralRenderingReuse::SampleAgrees(true, depth, depth, kCameraData, false, 1.0f, 1.0f));
	ASSERT(IsTrue, NeuralRenderingReuse::SampleAgrees(true, depth, depth, kCameraData, true, 1.0f, 1.0f));
	ASSERT(IsTrue, NeuralRenderingReuse::LumaAgrees(1.0f, 1.0f + NeuralRenderingReuse::kReuseLumaTolerance));
}

/// @tags neural-rendering
/// A sample that reprojected off screen is never reused
[numthreads(1, 1, 1)] void TestReuseRejectsAnOffScreenSample() {
	const float depth = RawDepthForDistance(250.0f, kCameraData);
	ASSERT(IsTrue, !NeuralRenderingReuse::SampleAgrees(false, depth, depth, kCameraData, false, 1.0f, 1.0f));
	ASSERT(IsTrue, !NeuralRenderingReuse::SampleAgrees(false, depth, depth, kCameraData, true, 1.0f, 1.0f));
}

	/// @tags neural-rendering
	/// A depth jump is a disocclusion: the stored gain belongs to another surface
	[numthreads(1, 1, 1)] void TestReuseRejectsADepthJump()
{
	const float near250 = RawDepthForDistance(250.0f, kCameraData);
	const float far258 = RawDepthForDistance(258.0f, kCameraData);
	const float far300 = RawDepthForDistance(300.0f, kCameraData);
	ASSERT(IsTrue, NeuralRenderingReuse::DepthAgrees(near250, far258, kCameraData, false));
	ASSERT(IsTrue, !NeuralRenderingReuse::DepthAgrees(near250, far300, kCameraData, false));
	ASSERT(IsTrue, NeuralRenderingReuse::DepthAgrees(1.0f - near250, 1.0f - far258, kCameraData, true));
	ASSERT(IsTrue, !NeuralRenderingReuse::DepthAgrees(1.0f - near250, 1.0f - far300, kCameraData, true));
}

/// @tags neural-rendering
/// A luminance jump is a lighting change: the stored gain no longer fits
[numthreads(1, 1, 1)] void TestReuseRejectsALumaJump() {
	const float depth = RawDepthForDistance(250.0f, kCameraData);
	ASSERT(IsTrue, !NeuralRenderingReuse::LumaAgrees(2.0f, 2.0f + NeuralRenderingReuse::kReuseLumaTolerance + 0.01f));
	ASSERT(IsTrue, !NeuralRenderingReuse::LumaAgrees(-2.0f, 2.0f));
	ASSERT(IsTrue, !NeuralRenderingReuse::SampleAgrees(true, depth, depth, kCameraData, false, 1.0f, 3.0f));
}

	/// @tags neural-rendering
	/// A non-finite depth or luminance is never reused
	[numthreads(1, 1, 1)] void TestReuseRejectsNonFiniteInputs()
{
	const float notANumber = asfloat(0x7FC00000u);
	const float positiveInfinity = asfloat(0x7F800000u);
	const float depth = RawDepthForDistance(250.0f, kCameraData);
	ASSERT(IsTrue, !NeuralRenderingReuse::DepthAgrees(notANumber, depth, kCameraData, false));
	ASSERT(IsTrue, !NeuralRenderingReuse::DepthAgrees(positiveInfinity, depth, kCameraData, false));
	ASSERT(IsTrue, !NeuralRenderingReuse::LumaAgrees(notANumber, 1.0f));
	ASSERT(IsTrue, !NeuralRenderingReuse::LumaAgrees(1.0f, positiveInfinity));
}

/// @tags neural-rendering
/// The reused proxy carries the requested luminance ratio for a sample colour
[numthreads(1, 1, 1)] void TestReuseGainReproducesTheRequestedLuminanceRatio() {
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float3 sampleColor = float3(0.18f, 0.32f, 0.55f);
	const float3 brighter = NeuralRenderingReuse::ApplyGainLinear(sampleColor, 0.5f);
	const float3 darker = NeuralRenderingReuse::ApplyGainLinear(sampleColor, -0.5f);
	const float3 unchanged = NeuralRenderingReuse::ApplyGainLinear(sampleColor, 0.0f);
	ASSERT(IsTrue, abs(dot(brighter, luma) / dot(sampleColor, luma) - exp2(0.5f)) < 1e-4f);
	ASSERT(IsTrue, abs(dot(darker, luma) / dot(sampleColor, luma) - exp2(-0.5f)) < 1e-4f);
	ASSERT(IsTrue, abs(dot(unchanged, luma) / dot(sampleColor, luma) - 1.0f) < 1e-4f);

	ASSERT(IsTrue, all(abs(unchanged - sampleColor) < 1e-6f));
	ASSERT(IsTrue, all(abs(brighter * sampleColor.y - sampleColor * brighter.y) < 1e-6f));
}
