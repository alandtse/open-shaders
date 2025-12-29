# HLSL Shader Tests

Comprehensive unit tests for Skyrim Community Shaders using [ShaderTestFramework](https://github.com/KStocky/ShaderTestFramework).

## Quick Start (Windows)

### Build and Run Tests

```powershell
# Configure project (includes tests)
cmake --preset ALL

# Build shader tests
cmake --build --preset Dev --target shader_tests

# Run all tests
ctest -R ShaderTests --output-on-failure

# Or run the test executable directly
.\build\ALL\tests\shaders\shader_tests.exe
```

### Run Specific Tests

```powershell
# Run tests by tag
.\build\ALL\tests\shaders\shader_tests.exe "[framebuffer]"
.\build\ALL\tests\shaders\shader_tests.exe "[vr]"
.\build\ALL\tests\shaders\shader_tests.exe "[non-vr]"

# Run specific test
.\build\ALL\tests\shaders\shader_tests.exe "TestIsOutsideFrameBasic"
```

## Test Structure

### Runtime Discovery

Tests are automatically discovered at runtime by scanning `package/Shaders/Tests/Test*.hlsl` files for functions marked with `[numthreads(1,1,1)]`.

No build-time code generation needed!

### Test Files

| File                              | Coverage                                | Tests          | Dependencies |
| --------------------------------- | --------------------------------------- | -------------- | ------------ |
| `TestMath.hlsl`                   | Math constants, epsilon values          | 3 tests        | None         |
| `TestBRDF.hlsl`                   | BRDF functions                          | Multiple tests | None         |
| `TestGBuffer.hlsl`                | GBuffer encoding/decoding               | 5 tests        | None         |
| `TestFrameBuffer.hlsl`            | FrameBuffer transforms, VR/non-VR       | 18 tests       | None         |
| `TestColorWithFixtures.hlsl`      | Color conversions, luminance, AO, gamma | 20 tests       | None         |
| `TestTextureOpsWithFixtures.hlsl` | Texture sampling, UAVs, processing      | 13 tests       | ✅ Fixtures  |
| `TestDefines.hlsl`                | Preprocessor define support             | 5 tests        | None         |
| _...more to come_                 |                                         |                |              |

### Current Coverage

-   ✅ **Math utilities** - Constants, epsilon values, matrices (6 tests)
-   ✅ **GBuffer** - Normal encoding/decoding, octahedral wrapping (5 tests)
-   ✅ **FrameBuffer** - Coordinate transforms, dynamic resolution, VR/non-VR variants (18 tests)
-   ✅ **Color** - Luminance, color spaces, saturation, AO, gamma (20 tests)
-   ✅ **Texture operations** - Sampling, UAVs, input/output (13 tests with fixtures)
-   ✅ **Preprocessor defines** - Conditional compilation support (5 tests)
-   ⏳ **Skylighting** - Probe sampling (needs advanced fixtures)
-   ⏳ **TerrainBlending** - Depth blending (needs advanced fixtures)
-   ⏳ **More features...**

**Total: ~67 tests across multiple production code modules**

## Writing Tests

### Basic Test (No Dependencies)

```hlsl
// TestMath.hlsl
#include "/Shaders/Common/Math.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags math, constants
[numthreads(1, 1, 1)]
void TestPIConstant()
{
    ASSERT(AreEqual, Math::PI, 3.14159265359f);
    ASSERT(IsTrue, Math::PI > 3.0);
}
```

### Test with Preprocessor Defines

```hlsl
// TestFrameBuffer.hlsl
#include "/Shaders/Common/FrameBuffer.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags framebuffer, vr
/// @define VR  // Enables VR-specific code paths
[numthreads(1, 1, 1)]
void TestVRDynamicResolution()
{
    // VR-specific code is now available!
#if defined(VR)
    // Test VR stereo clamping
    float2 rightEye = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(
        float2(0.75, 0.5),
        1  // stereo = 1
    );
    ASSERT(IsTrue, rightEye.x >= 0.5);
#endif
}
```

### Test with Resource Bindings (Future - YAML Fixtures)

```hlsl
/// @tags skylighting, pixel-shader
/// @define PSHADER
/// @fixture(t50) SkylightingProbe3D
/// @fixture(t51) BlueNoise2DArray

#include "/Shaders/Skylighting/Skylighting.hlsli"

[numthreads(1,1,1)]
void TestSkylightingSample() {
    // Production code works with mocked resources!
    sh2 result = Skylighting::sample(...);
    ASSERT(IsTrue, isfinite(result.x));
}
```

## Annotations Reference

### `@tags`

Categorize tests for filtering:

```hlsl
/// @tags math, constants
/// @tags framebuffer, vr, dynamic-resolution
```

Run specific tags:

```powershell
.\shader_tests.exe "[vr]"
.\shader_tests.exe "[math]"
```

### `@define`

Set preprocessor defines for conditional compilation:

```hlsl
/// @define PSHADER     // Pixel shader variant
/// @define VR          // VR variant
/// @define LIGHTING    // Lighting enabled
```

Allows testing different shader variants:

-   Pixel shader vs compute shader paths
-   VR vs non-VR code
-   Feature flags (lighting, shadows, etc.)

### `@fixture` (Coming Soon)

Reference YAML-defined mock resources:

```hlsl
/// @fixture(t0) GradientTexture8x8
/// @fixture(b0) CommonCBuffer
/// @fixture(s0) LinearClampSampler
```

## Assertions

STF provides several assertion macros:

```hlsl
// Equality
ASSERT(AreEqual, actual, expected);
ASSERT(AreNotEqual, value1, value2);

// Boolean
ASSERT(IsTrue, condition);
ASSERT(IsFalse, condition);

// Floating point (with tolerance)
ASSERT(AreEqualWithTolerance, actual, expected, tolerance);

// Scenarios (BDD-style, optional)
SCENARIO("Description of test scenario")
{
    SECTION("Given some condition")
    {
        // Setup
        SECTION("When something happens")
        {
            // Action
            SECTION("Then expect result")
            {
                ASSERT(IsTrue, result);
            }
        }
    }
}
```

## Test Organization

```
tests/shaders/
├── CMakeLists.txt              # Test build configuration
├── minimal_test.cpp            # Test entry point (main)
├── runtime_discovered_tests.cpp # Runtime test discovery
├── runtime_test_discovery.h    # HLSL scanning logic
├── test_common.h               # Common test utilities
├── test_helpers_unified.h      # Helper macros
└── fixtures/                   # YAML fixture library (future)
    ├── README.md
    ├── common_fixtures.yaml
    └── ...

package/Shaders/Tests/          # HLSL test files
├── TestMath.hlsl
├── TestGBuffer.hlsl
├── TestFrameBuffer.hlsl
├── TestDefines.hlsl
└── ...
```

## CI Integration

Tests run automatically before packaging:

```yaml
# .github/workflows/build.yml
- name: Run Shader Tests
  run: ctest -R ShaderTests --output-on-failure
```

## Debugging Tests

### View Detailed Test Output

```powershell
# Run with verbose output
.\shader_tests.exe -s

# Run specific test with full details
.\shader_tests.exe "TestFrameBuffer - Dynamic Resolution" -s
```

### Common Issues

**Issue**: Test not discovered

-   Check filename starts with `Test*.hlsl`
-   Check function has `[numthreads(1,1,1)]` attribute
-   Check function signature is `void FunctionName()`

**Issue**: Compilation errors

-   Verify includes use `/Shaders/` prefix
-   Check that required defines are set with `@define`
-   Ensure included HLSL is compatible with compute shader context

**Issue**: cbuffer/texture not available

-   For now, cbuffer tests require mock data in the test itself
-   Full fixture support coming soon (YAML-based)

## Examples

### Testing Math Utilities

See `TestMath.hlsl` for examples of testing pure functions with no dependencies.

### Testing Conditional Compilation

See `TestFrameBuffer.hlsl` for VR/non-VR variants using `@define`.

### Testing Define Support

See `TestDefines.hlsl` for comprehensive `@define` annotation examples.

## Roadmap

-   [x] Runtime test discovery
-   [x] Preprocessor define support (`@define`)
-   [x] Tag-based test filtering
-   [x] Basic assertion framework
-   [x] Tests for Math, GBuffer, FrameBuffer, Color
-   [x] C++ fixture system (textures, UAVs, samplers)
-   [x] Fixture-based texture operation tests
-   [x] YAML fixture parser (simple implementation)
-   [ ] Advanced YAML fixtures (cbuffers, 3D textures)
-   [ ] Fixture integration with runtime discovery
-   [ ] Tests for Skylighting, TerrainBlending with fixtures
-   [ ] Performance benchmarking
-   [ ] Contribute enhancements to STF upstream

## References

-   [ShaderTestFramework](https://github.com/KStocky/ShaderTestFramework)
-   [STF Tutorial](https://github.com/KStocky/ShaderTestFramework/blob/main/docs/Tutorial.md)
-   [Catch2 Documentation](https://github.com/catchorg/Catch2/tree/devel/docs)
-   [LLVM YAML Pipeline Format](https://github.com/llvm/offload-test-suite) (basis for fixtures)
