// HLSL Unit Tests demonstrating preprocessor define support
// Shows how to test code with conditional compilation
#include "/Test/STF/ShaderTestFramework.hlsli"

// Test 1: Basic define usage
/// @tags defines, basic
/// @define TEST_MODE
[numthreads(1, 1, 1)]
void TestBasicDefine()
{
#if defined(TEST_MODE)
    const int expectedValue = 42;
#else
    const int expectedValue = 0;
#endif

    // This should pass because @define TEST_MODE is set
    ASSERT(AreEqual, expectedValue, 42);
}

// Test 2: Multiple defines
/// @tags defines, multiple
/// @define PSHADER
/// @define VR
[numthreads(1, 1, 1)]
void TestMultipleDefines()
{
    int value = 0;

#if defined(PSHADER)
    value += 10;
#endif

#if defined(VR)
    value += 5;
#endif

    // Should be 15 (both defines active)
    ASSERT(AreEqual, value, 15);
}

// Test 3: Define affects resource availability (like real production code)
/// @tags defines, resources
/// @define PSHADER
[numthreads(1, 1, 1)]
void TestConditionalResources()
{
#if defined(PSHADER)
    // In pixel shaders, this constant would exist
    const bool isPixelShader = true;
#else
    const bool isPixelShader = false;
#endif

    ASSERT(IsTrue, isPixelShader);
}

// Test 4: No defines (control test)
/// @tags defines, control
[numthreads(1, 1, 1)]
void TestNoDefines()
{
    // Without any @define annotations, nothing should be defined
#if defined(TEST_MODE)
    const bool testModeActive = true;
#else
    const bool testModeActive = false;
#endif

    ASSERT(IsFalse, testModeActive);
}

// Test 5: Simulating shader variant selection
/// @tags defines, variant, pixel-shader
/// @define PSHADER
/// @define LIGHTING
/// @define SPECULAR
[numthreads(1, 1, 1)]
void TestShaderVariant()
{
    // Simulate feature flags that production code uses
    int featureFlags = 0;

#if defined(PSHADER)
    featureFlags |= 0x01;  // Pixel shader bit
#endif

#if defined(LIGHTING)
    featureFlags |= 0x02;  // Lighting bit
#endif

#if defined(SPECULAR)
    featureFlags |= 0x04;  // Specular bit
#endif

    // Should have all three bits set: 0x07 = 0b111
    ASSERT(AreEqual, featureFlags, 0x07);
}
