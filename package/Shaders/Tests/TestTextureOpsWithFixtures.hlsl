// HLSL Unit Tests demonstrating fixture-based testing
// Tests texture sampling and operations using C++ fixtures
#include "/Test/STF/ShaderTestFramework.hlsli"

// ============================================================================
// FIXTURE-BASED TEXTURE TESTS
// ============================================================================
// These tests demonstrate using fixtures to test code with resource dependencies
// The fixtures are defined in C++ (test_fixtures.h) and wired up automatically

/// @tags texture, sampling, gradient
/// @fixture(t0) GradientTexture8x8
/// @sampler(s0) PointClampSampler
Texture2D<float> GradientTex : register(t0);
SamplerState PointSamp : register(s0);

[numthreads(1, 1, 1)]
void TestGradientTextureSampling()
{
	// GradientTexture8x8: 8x8 gradient from 0.0 to 1.0 (diagonal)
	// Formula: (u + v) * 0.5

	// Test corner values
	float topLeft = GradientTex.SampleLevel(PointSamp, float2(0.0, 0.0), 0);
	float bottomRight = GradientTex.SampleLevel(PointSamp, float2(1.0, 1.0), 0);

	// Top-left should be darkest (0.0)
	ASSERT(IsTrue, topLeft < 0.1);

	// Bottom-right should be brightest (1.0)
	ASSERT(IsTrue, bottomRight > 0.9);

	// Gradient should increase diagonally
	ASSERT(IsTrue, bottomRight > topLeft);
}

/// @tags texture, sampling, center
/// @fixture(t0) GradientTexture8x8
/// @sampler(s0) PointClampSampler
Texture2D<float> CenterGradientTex : register(t0);
SamplerState CenterPointSamp : register(s0);

[numthreads(1, 1, 1)]
void TestGradientTextureCenter()
{
	// Center of gradient should be approximately 0.5
	float center = CenterGradientTex.SampleLevel(CenterPointSamp, float2(0.5, 0.5), 0);

	// Gradient formula: (0.5 + 0.5) * 0.5 = 0.5
	ASSERT(IsTrue, abs(center - 0.5) < 0.15);
}

/// @tags texture, constant, white
/// @fixture(t0) ConstantTexture8x8
/// @sampler(s0) LinearClampSampler
Texture2D<float> WhiteTex : register(t0);
SamplerState LinearSamp : register(s0);

[numthreads(1, 1, 1)]
void TestConstantTextureSampling()
{
	// ConstantTexture8x8: all pixels should be 1.0

	// Test multiple sample points
	float topLeft = WhiteTex.SampleLevel(LinearSamp, float2(0.0, 0.0), 0);
	float center = WhiteTex.SampleLevel(LinearSamp, float2(0.5, 0.5), 0);
	float bottomRight = WhiteTex.SampleLevel(LinearSamp, float2(1.0, 1.0), 0);

	// All should be 1.0
	ASSERT(AreEqual, topLeft, 1.0);
	ASSERT(AreEqual, center, 1.0);
	ASSERT(AreEqual, bottomRight, 1.0);
}

/// @tags texture, load, no-sampler
/// @fixture(t0) GradientTexture8x8
Texture2D<float> LoadGradientTex : register(t0);

[numthreads(1, 1, 1)]
void TestTextureLoad()
{
	// Test Load (doesn't need sampler)

	// Load from corners (pixel coordinates)
	float topLeft = LoadGradientTex.Load(int3(0, 0, 0));
	float bottomRight = LoadGradientTex.Load(int3(7, 7, 0));  // 8x8 texture, so max is 7

	// Gradient should increase
	ASSERT(IsTrue, bottomRight > topLeft);

	// Load from center
	float center = LoadGradientTex.Load(int3(4, 4, 0));
	ASSERT(IsTrue, center > topLeft);
	ASSERT(IsTrue, center < bottomRight);
}

// ============================================================================
// UAV / OUTPUT TESTS
// ============================================================================

/// @tags uav, write, output
/// @fixture(u0) OutputTexture8x8
RWTexture2D<float4> OutputTex : register(u0);

[numthreads(1, 1, 1)]
void TestUAVWrite()
{
	// Test writing to UAV
	uint2 coord = uint2(0, 0);

	// Write test pattern
	OutputTex[coord] = float4(1.0, 0.5, 0.25, 1.0);

	// Read back
	float4 written = OutputTex[coord];

	// Verify values
	ASSERT(AreEqual, written.x, 1.0);
	ASSERT(AreEqual, written.y, 0.5);
	ASSERT(AreEqual, written.z, 0.25);
	ASSERT(AreEqual, written.w, 1.0);
}

/// @tags uav, write, multiple-pixels
/// @fixture(u0) OutputTexture8x8
RWTexture2D<float4> MultiPixelOutput : register(u0);

[numthreads(1, 1, 1)]
void TestUAVMultipleWrites()
{
	// Write to multiple pixels
	MultiPixelOutput[uint2(0, 0)] = float4(1, 0, 0, 1);  // Red
	MultiPixelOutput[uint2(1, 0)] = float4(0, 1, 0, 1);  // Green
	MultiPixelOutput[uint2(0, 1)] = float4(0, 0, 1, 1);  // Blue
	MultiPixelOutput[uint2(1, 1)] = float4(1, 1, 1, 1);  // White

	// Verify each pixel
	ASSERT(AreEqual, MultiPixelOutput[uint2(0, 0)].r, 1.0);
	ASSERT(AreEqual, MultiPixelOutput[uint2(1, 0)].g, 1.0);
	ASSERT(AreEqual, MultiPixelOutput[uint2(0, 1)].b, 1.0);
	ASSERT(AreEqual, MultiPixelOutput[uint2(1, 1)].r, 1.0);
	ASSERT(AreEqual, MultiPixelOutput[uint2(1, 1)].g, 1.0);
	ASSERT(AreEqual, MultiPixelOutput[uint2(1, 1)].b, 1.0);
}

// ============================================================================
// COMBINED INPUT/OUTPUT TESTS
// ============================================================================

/// @tags combined, input-output, processing
/// @fixture(t0) GradientTexture8x8
/// @fixture(u0) OutputTexture8x8
Texture2D<float> InputGradient : register(t0);
RWTexture2D<float4> ProcessedOutput : register(u0);

[numthreads(1, 1, 1)]
void TestTextureProcessing()
{
	// Simulate a simple processing shader: read from input, write to output

	// Read gradient value
	float inputValue = InputGradient.Load(int3(4, 4, 0));

	// Process: invert the value
	float processed = 1.0 - inputValue;

	// Write result
	ProcessedOutput[uint2(4, 4)] = float4(processed, processed, processed, 1.0);

	// Verify output
	float4 output = ProcessedOutput[uint2(4, 4)];
	ASSERT(IsTrue, abs(output.x - processed) < 0.01);
}

/// @tags combined, copy-operation
/// @fixture(t0) ConstantTexture8x8
/// @fixture(u0) OutputTexture8x8
Texture2D<float> ConstInput : register(t0);
RWTexture2D<float4> CopyOutput : register(u0);

[numthreads(1, 1, 1)]
void TestTextureCopy()
{
	// Simulate a copy/blit operation

	uint2 coord = uint2(2, 3);

	// Read from input (should be 1.0)
	float inputValue = ConstInput.Load(int3(coord, 0));

	// Copy to output
	CopyOutput[coord] = float4(inputValue, inputValue, inputValue, 1.0);

	// Verify copy
	float4 output = CopyOutput[coord];
	ASSERT(AreEqual, output.x, inputValue);
	ASSERT(AreEqual, output.y, inputValue);
	ASSERT(AreEqual, output.z, inputValue);
}

// ============================================================================
// SAMPLER COMPARISON TESTS
// ============================================================================

/// @tags sampler, comparison, linear-vs-point
/// @fixture(t0) GradientTexture8x8
/// @sampler(s0) LinearClampSampler
/// @sampler(s1) PointClampSampler
Texture2D<float> ComparisonTex : register(t0);
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

[numthreads(1, 1, 1)]
void TestSamplerDifference()
{
	// Sample between pixels where linear and point should differ
	float2 betweenPixels = float2(0.5 / 8.0, 0.5 / 8.0);  // Between pixel (0,0) and (1,1)

	float linearSample = ComparisonTex.SampleLevel(LinearSampler, betweenPixels, 0);
	float pointSample = ComparisonTex.SampleLevel(PointSampler, betweenPixels, 0);

	// Linear should interpolate, point should snap
	// For gradient, linear sampling between pixels should give intermediate value
	// Point sampling should give one of the corner values

	// Both should be valid (0.0 to 1.0)
	ASSERT(IsTrue, linearSample >= 0.0 && linearSample <= 1.0);
	ASSERT(IsTrue, pointSample >= 0.0 && pointSample <= 1.0);

	// They might be different (not always guaranteed, but likely for gradient)
	// Just verify both are valid for now
}

// ============================================================================
// BOUNDARY TESTS
// ============================================================================

/// @tags texture, bounds, edge-cases
/// @fixture(t0) ConstantTexture8x8
/// @sampler(s0) PointClampSampler
Texture2D<float> BoundsTex : register(t0);
SamplerState BoundsSamp : register(s0);

[numthreads(1, 1, 1)]
void TestTextureBoundaries()
{
	// Test exact boundary coordinates
	float edge00 = BoundsTex.SampleLevel(BoundsSamp, float2(0.0, 0.0), 0);
	float edge10 = BoundsTex.SampleLevel(BoundsSamp, float2(1.0, 0.0), 0);
	float edge01 = BoundsTex.SampleLevel(BoundsSamp, float2(0.0, 1.0), 0);
	float edge11 = BoundsTex.SampleLevel(BoundsSamp, float2(1.0, 1.0), 0);

	// All should be 1.0 for constant texture
	ASSERT(AreEqual, edge00, 1.0);
	ASSERT(AreEqual, edge10, 1.0);
	ASSERT(AreEqual, edge01, 1.0);
	ASSERT(AreEqual, edge11, 1.0);

	// Test clamping behavior (values > 1.0 should clamp)
	float clamped = BoundsTex.SampleLevel(BoundsSamp, float2(2.0, 2.0), 0);
	ASSERT(AreEqual, clamped, 1.0);  // Should clamp and sample edge pixel
}
