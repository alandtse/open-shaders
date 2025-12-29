// HLSL Unit Tests for Texture Operations
// Demonstrates fixture-based testing with automatic resource binding
#include "/Test/STF/ShaderTestFramework.hlsli"

// ============================================================================
// FIXTURE-BASED TESTS
// ============================================================================
// These tests use the @fixture annotation to automatically wire up resources
// No C++ code required beyond the fixture definition!

/// @tags texture, sampling
/// @fixture(t0) GradientTexture2D
/// @fixture(s0) LinearClampSampler
Texture2D<float> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);

[numthreads(1, 1, 1)]
void TestTextureSampling()
{
    // Sample at the center of the texture
    float2 uv = float2(0.5, 0.5);
    float value = InputTexture.SampleLevel(LinearSampler, uv, 0);

    // GradientTexture2D produces values from 0.0 to 1.0
    // At center (0.5, 0.5), we expect approximately 0.5
    ASSERT(IsTrue, value >= 0.0 && value <= 1.0);
    ASSERT(IsTrue, abs(value - 0.5) < 0.2);  // Allow some tolerance
}

/// @tags texture, sampling, corners
/// @fixture(t0) GradientTexture2D
/// @fixture(s0) PointClampSampler
Texture2D<float> CornerTexture : register(t0);
SamplerState PointSampler : register(s0);

[numthreads(1, 1, 1)]
void TestTextureCorners()
{
    // Test four corners of the gradient texture
    float topLeft = CornerTexture.SampleLevel(PointSampler, float2(0.0, 0.0), 0);
    float topRight = CornerTexture.SampleLevel(PointSampler, float2(1.0, 0.0), 0);
    float bottomLeft = CornerTexture.SampleLevel(PointSampler, float2(0.0, 1.0), 0);
    float bottomRight = CornerTexture.SampleLevel(PointSampler, float2(1.0, 1.0), 0);

    // Gradient should increase from top-left to bottom-right
    ASSERT(IsTrue, topLeft < bottomRight);
    ASSERT(IsTrue, topRight > topLeft);
    ASSERT(IsTrue, bottomLeft > topLeft);
}

// ============================================================================
// CBUFFER TESTS
// ============================================================================

/// @tags cbuffer, constants
/// @fixture(b0) CommonCBuffer
cbuffer TestData : register(b0)
{
    float time;
    float deltaTime;
    float frameIndex;
    float _padding;
};

[numthreads(1, 1, 1)]
void TestCBufferAccess()
{
    // CommonCBuffer defaults: time=0.0, deltaTime=0.016, frameIndex=0.0
    ASSERT(AreEqual, time, 0.0f);
    ASSERT(IsTrue, abs(deltaTime - 0.016f) < 0.001f);
    ASSERT(AreEqual, frameIndex, 0.0f);
}

// ============================================================================
// COMBINED TESTS (multiple fixtures)
// ============================================================================

/// @tags texture, cbuffer, combined
/// @fixture(t0) ConstantTexture2D
/// @fixture(b0) CommonCBuffer
/// @fixture(s0) PointClampSampler
Texture2D<float> CombinedTexture : register(t0);
SamplerState CombinedSampler : register(s0);

cbuffer CombinedData : register(b0)
{
    float testTime;
    float testDelta;
    float testFrame;
    float _pad;
};

[numthreads(1, 1, 1)]
void TestCombinedFixtures()
{
    // Test that we can access both texture and cbuffer
    float texValue = CombinedTexture.SampleLevel(CombinedSampler, float2(0.5, 0.5), 0);

    // ConstantTexture2D defaults to 1.0
    ASSERT(AreEqual, texValue, 1.0f);

    // CommonCBuffer should also be accessible
    ASSERT(AreEqual, testTime, 0.0f);
}

// ============================================================================
// UAV/OUTPUT TESTS
// ============================================================================

/// @tags uav, output, write
/// @fixture(u0) OutputTexture2D
RWTexture2D<float4> OutputTex : register(u0);

[numthreads(1, 1, 1)]
void TestUAVWrite()
{
    // Write to output texture
    uint2 coord = uint2(0, 0);
    OutputTex[coord] = float4(1.0, 0.5, 0.25, 1.0);

    // Read back and verify
    float4 written = OutputTex[coord];
    ASSERT(AreEqual, written.x, 1.0f);
    ASSERT(AreEqual, written.y, 0.5f);
    ASSERT(AreEqual, written.z, 0.25f);
    ASSERT(AreEqual, written.w, 1.0f);
}
