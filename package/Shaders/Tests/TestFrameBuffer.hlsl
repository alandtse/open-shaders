// HLSL Unit Tests for Common/FrameBuffer.hlsli functions
// NOTE: We cannot include FrameBuffer.hlsli directly due to large constant buffer size.
// Instead, we test individual functions in isolation.
//
// Most FrameBuffer functions (dynamic resolution, motion vectors) require the full
// PerFrame constant buffer which is too large for ShaderTestFramework's root constants.
// Those functions should be tested via integration/rendering validation.
//
// Here we test only the simple utility functions that can work in isolation.

#include "/Test/STF/ShaderTestFramework.hlsli"

// Test tolerance constants
namespace TestConstants
{
    static const float FLOAT16_EPSILON = 0.001f;
    static const float APPROX_TOLERANCE = 0.01f;
    static const float EXACT_TOLERANCE = 0.0001f;
    static const float NEAR_ZERO = 0.0001f;
}

// ============================================================================
// Isolated function copies for testing (to avoid CB dependency)
// ============================================================================

namespace FrameBufferTest
{
    // Mock constant buffer value for ToSRGBColor testing
    static const float4 FrameParams = float4(1.0 / 2.2, 0, 0, 0); // inverse gamma

    // Copied from FrameBuffer.hlsli - ToSRGBColor function
    float3 ToSRGBColor(float3 colorInput)
    {
        return pow(abs(colorInput), FrameParams.xxx);
    }

    // Copied from FrameBuffer.hlsli - IsOutsideFrame function (simplified)
    // Original uses dynamic resolution params, we test with explicit max
    bool IsOutsideFrame(float2 uv, float2 maxUV)
    {
        return any(uv < float2(0, 0)) || any(uv > maxUV);
    }
}

// ============================================================================
// Tests - IsOutsideFrame
// ============================================================================

/// @tags framebuffer, bounds-checking
[numthreads(1, 1, 1)]
void TestIsOutsideFrameBasic()
{
    float2 maxUV = float2(1.0, 1.0);

    // Test inside points
    ASSERT(IsFalse, FrameBufferTest::IsOutsideFrame(float2(0.5, 0.5), maxUV));
    ASSERT(IsFalse, FrameBufferTest::IsOutsideFrame(float2(0.0, 0.0), maxUV));
    ASSERT(IsFalse, FrameBufferTest::IsOutsideFrame(float2(1.0, 1.0), maxUV));

    // Test outside points
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(-0.1, 0.5), maxUV));
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(0.5, -0.1), maxUV));
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(1.1, 0.5), maxUV));
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(0.5, 1.1), maxUV));
}

/// @tags framebuffer, bounds-checking, edge-cases
[numthreads(1, 1, 1)]
void TestIsOutsideFrameEdgeCases()
{
    float2 maxUV = float2(1.0, 1.0);

    // Test extreme values
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(10.0, 10.0), maxUV));
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(-10.0, -10.0), maxUV));
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(1e38, 1e38), maxUV));

    // Test mixed in/out
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(2.0, 0.5), maxUV));
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(0.5, 2.0), maxUV));
}

/// @tags framebuffer, bounds, properties
[numthreads(1, 1, 1)]
void TestIsOutsideFrameProperties()
{
    // Property: Monotonicity - larger max should contain more points
    float2 testPoint = float2(1.5, 0.5);
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(testPoint, float2(1.0, 1.0)));
    ASSERT(IsFalse, FrameBufferTest::IsOutsideFrame(testPoint, float2(2.0, 2.0)));

    // Property: Symmetry
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(-0.5, 0.5), float2(1.0, 1.0)));
    ASSERT(IsTrue, FrameBufferTest::IsOutsideFrame(float2(0.5, -0.5), float2(1.0, 1.0)));
}

// ============================================================================
// Tests - ToSRGBColor
// ============================================================================

/// @tags framebuffer, color, srgb, gamma
[numthreads(1, 1, 1)]
void TestToSRGBColor()
{
    // Test basic gamma conversion
    float3 srgbResult = FrameBufferTest::ToSRGBColor(float3(0.5, 0.5, 0.5));
    ASSERT(IsTrue, all(!isnan(srgbResult)));
    ASSERT(IsTrue, all(!isinf(srgbResult)));
    ASSERT(IsTrue, all(srgbResult >= 0.0));

    // Test zero
    float3 srgbZero = FrameBufferTest::ToSRGBColor(float3(0.0, 0.0, 0.0));
    ASSERT(IsTrue, all(abs(srgbZero) < TestConstants::EXACT_TOLERANCE));

    // Test one
    float3 srgbOne = FrameBufferTest::ToSRGBColor(float3(1.0, 1.0, 1.0));
    ASSERT(IsTrue, all(!isnan(srgbOne)));
    ASSERT(IsTrue, all(srgbOne >= 0.0));
}

/// @tags framebuffer, color, srgb, edge-cases
[numthreads(1, 1, 1)]
void TestToSRGBColorEdgeCases()
{
    // Test negative values (abs() handles them)
    float3 srgbNeg = FrameBufferTest::ToSRGBColor(float3(-0.5, -0.5, -0.5));
    ASSERT(IsTrue, all(!isnan(srgbNeg)));
    ASSERT(IsTrue, all(srgbNeg >= 0.0));

    // Test very small values
    float3 srgbTiny = FrameBufferTest::ToSRGBColor(float3(0.001, 0.001, 0.001));
    ASSERT(IsTrue, all(!isnan(srgbTiny)));
    ASSERT(IsTrue, all(srgbTiny >= 0.0));

    // Test HDR values
    float3 srgbHDR = FrameBufferTest::ToSRGBColor(float3(2.0, 3.0, 5.0));
    ASSERT(IsTrue, all(!isnan(srgbHDR)));
    ASSERT(IsTrue, all(srgbHDR >= 0.0));
}

/// @tags framebuffer, color, gamma, properties
[numthreads(1, 1, 1)]
void TestToSRGBColorProperties()
{
    // Property: Monotonicity - brighter input gives brighter output
    float3 srgbDark = FrameBufferTest::ToSRGBColor(float3(0.2, 0.2, 0.2));
    float3 srgbBright = FrameBufferTest::ToSRGBColor(float3(0.8, 0.8, 0.8));
    ASSERT(IsTrue, all(srgbBright > srgbDark));

    // Property: Homogeneity - larger input gives larger output
    float3 srgbHalf = FrameBufferTest::ToSRGBColor(float3(0.5, 0.5, 0.5));
    float3 srgbFull = FrameBufferTest::ToSRGBColor(float3(1.0, 1.0, 1.0));
    ASSERT(IsTrue, all(srgbFull > srgbHalf));
}
