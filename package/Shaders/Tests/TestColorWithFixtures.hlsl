// HLSL Unit Tests for Common/Color.hlsli
// Tests color space conversions, luminance calculations, and AO functions
#include "/Shaders/Common/Color.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

// ============================================================================
// LUMINANCE TESTS
// ============================================================================

/// @tags color, luminance, basic
[numthreads(1, 1, 1)]
void TestRGBToLuminancePure()
{
	// Test pure colors
	float3 red = float3(1, 0, 0);
	float3 green = float3(0, 1, 0);
	float3 blue = float3(0, 0, 1);

	float lumRed = Color::RGBToLuminance(red);
	float lumGreen = Color::RGBToLuminance(green);
	float lumBlue = Color::RGBToLuminance(blue);

	// Green should be brightest (coefficient 0.7154)
	ASSERT(IsTrue, lumGreen > lumRed);
	ASSERT(IsTrue, lumGreen > lumBlue);

	// Red should be brighter than blue (0.2125 vs 0.0721)
	ASSERT(IsTrue, lumRed > lumBlue);
}

/// @tags color, luminance, grayscale
[numthreads(1, 1, 1)]
void TestRGBToLuminanceGrayscale()
{
	// Test grayscale (should equal the gray value)
	float3 gray50 = float3(0.5, 0.5, 0.5);
	float lum = Color::RGBToLuminance(gray50);

	// Dot product of (0.5, 0.5, 0.5) with weights should equal 0.5
	// (0.2125 + 0.7154 + 0.0721) = 1.0, so 0.5 * 1.0 = 0.5
	ASSERT(IsTrue, abs(lum - 0.5) < 0.01);

	// Test extremes
	float3 white = float3(1, 1, 1);
	float3 black = float3(0, 0, 0);

	ASSERT(IsTrue, abs(Color::RGBToLuminance(white) - 1.0) < 0.01);
	ASSERT(AreEqual, Color::RGBToLuminance(black), 0.0);
}

/// @tags color, luminance, alternative-formulas
[numthreads(1, 1, 1)]
void TestLuminanceFormulasConsistency()
{
	// Different luminance formulas should produce similar but not identical results
	float3 testColor = float3(0.8, 0.3, 0.5);

	float lum1 = Color::RGBToLuminance(testColor);
	float lum2 = Color::RGBToLuminanceAlternative(testColor);
	float lum3 = Color::RGBToLuminance2(testColor);

	// All should be in valid range
	ASSERT(IsTrue, lum1 >= 0.0 && lum1 <= 1.0);
	ASSERT(IsTrue, lum2 >= 0.0 && lum2 <= 1.0);
	ASSERT(IsTrue, lum3 >= 0.0 && lum3 <= 1.0);

	// They should be similar (within 20% of each other)
	ASSERT(IsTrue, abs(lum1 - lum2) < 0.2);
	ASSERT(IsTrue, abs(lum1 - lum3) < 0.2);
}

// ============================================================================
// COLOR SPACE CONVERSION TESTS
// ============================================================================

/// @tags color, color-space, ycocg, roundtrip
[numthreads(1, 1, 1)]
void TestRGBYCoCgRoundtrip()
{
	// Test RGB -> YCoCg -> RGB roundtrip
	float3 originalColor = float3(0.8, 0.3, 0.5);

	float3 ycocg = Color::RGBToYCoCg(originalColor);
	float3 reconstructed = Color::YCoCgToRGB(ycocg);

	// Should reconstruct original color (within tolerance)
	ASSERT(IsTrue, abs(reconstructed.r - originalColor.r) < 0.01);
	ASSERT(IsTrue, abs(reconstructed.g - originalColor.g) < 0.01);
	ASSERT(IsTrue, abs(reconstructed.b - originalColor.b) < 0.01);
}

/// @tags color, color-space, ycocg, pure-colors
[numthreads(1, 1, 1)]
void TestRGBYCoCgPureColors()
{
	// Test pure colors
	float3 red = float3(1, 0, 0);
	float3 green = float3(0, 1, 0);
	float3 blue = float3(0, 0, 1);

	// Convert and verify roundtrip
	ASSERT(IsTrue, distance(Color::YCoCgToRGB(Color::RGBToYCoCg(red)), red) < 0.01);
	ASSERT(IsTrue, distance(Color::YCoCgToRGB(Color::RGBToYCoCg(green)), green) < 0.01);
	ASSERT(IsTrue, distance(Color::YCoCgToRGB(Color::RGBToYCoCg(blue)), blue) < 0.01);
}

/// @tags color, color-space, ycocg, grayscale
[numthreads(1, 1, 1)]
void TestYCoCgGrayscale()
{
	// Grayscale colors should have Co=0, Cg=0
	float3 gray = float3(0.5, 0.5, 0.5);
	float3 ycocg = Color::RGBToYCoCg(gray);

	// Y should equal luminance, Co and Cg should be near zero
	ASSERT(IsTrue, abs(ycocg.y) < 0.01);  // Co
	ASSERT(IsTrue, abs(ycocg.z) < 0.01);  // Cg
}

// ============================================================================
// SATURATION TESTS
// ============================================================================

/// @tags color, saturation, desaturate
[numthreads(1, 1, 1)]
void TestSaturationDesaturate()
{
	// Test desaturation (saturation = 0 should give grayscale)
	float3 colorful = float3(1.0, 0.2, 0.4);
	float3 desaturated = Color::Saturation(colorful, 0.0);

	float lum = Color::RGBToLuminance(colorful);

	// All channels should be equal (grayscale)
	ASSERT(IsTrue, abs(desaturated.r - desaturated.g) < 0.01);
	ASSERT(IsTrue, abs(desaturated.g - desaturated.b) < 0.01);

	// Should be close to luminance value
	ASSERT(IsTrue, abs(desaturated.r - lum) < 0.01);
}

/// @tags color, saturation, preserve
[numthreads(1, 1, 1)]
void TestSaturationPreserve()
{
	// Test saturation = 1.0 (should preserve original color)
	float3 original = float3(0.8, 0.3, 0.5);
	float3 saturated = Color::Saturation(original, 1.0);

	// Should be unchanged
	ASSERT(IsTrue, abs(saturated.r - original.r) < 0.01);
	ASSERT(IsTrue, abs(saturated.g - original.g) < 0.01);
	ASSERT(IsTrue, abs(saturated.b - original.b) < 0.01);
}

/// @tags color, saturation, oversaturate
[numthreads(1, 1, 1)]
void TestSaturationOversaturate()
{
	// Test oversaturation (saturation > 1.0)
	float3 original = float3(0.8, 0.3, 0.5);
	float3 oversaturated = Color::Saturation(original, 2.0);

	// Should be more saturated (higher contrast from gray)
	float lum = Color::RGBToLuminance(original);

	// Red channel (above lum) should be pushed further up
	ASSERT(IsTrue, oversaturated.r > original.r);

	// All values should be clamped to [0, inf) due to max(lerp(...), 0.0)
	ASSERT(IsTrue, all(oversaturated >= 0.0));
}

// ============================================================================
// AMBIENT OCCLUSION TESTS
// ============================================================================

/// @tags color, ao, multi-bounce
[numthreads(1, 1, 1)]
void TestMultiBounceAO()
{
	// Test multi-bounce AO [Jimenez et al. 2016]
	float3 baseColor = float3(0.5, 0.5, 0.5);
	float ao = 0.5;

	float3 result = Color::MultiBounceAO(baseColor, ao);

	// Result should be at least as bright as direct AO
	// (multi-bounce accounts for indirect lighting)
	ASSERT(IsTrue, all(result >= ao));

	// Result should be finite and positive
	ASSERT(IsTrue, all(isfinite(result)));
	ASSERT(IsTrue, all(result >= 0.0));
}

/// @tags color, ao, multi-bounce, extremes
[numthreads(1, 1, 1)]
void TestMultiBounceAOExtremes()
{
	float3 white = float3(1, 1, 1);
	float3 black = float3(0, 0, 0);

	// Full AO (ao = 1.0) with white base
	float3 fullAOWhite = Color::MultiBounceAO(white, 1.0);
	ASSERT(IsTrue, all(fullAOWhite >= 1.0));  // Should be at least fully lit

	// No AO (ao = 0.0) with any base
	float3 noAO = Color::MultiBounceAO(white, 0.0);
	// Should still respect the max(ao, ...) behavior

	// Black base color
	float3 blackBase = Color::MultiBounceAO(black, 0.5);
	ASSERT(IsTrue, all(isfinite(blackBase)));
}

/// @tags color, ao, specular, lagarde
[numthreads(1, 1, 1)]
void TestSpecularAOLagarde()
{
	// Test specular AO [Lagarde et al. 2014]
	float NdotV = 0.8;
	float ao = 0.5;
	float roughness = 0.3;

	float specAO = Color::SpecularAOLagarde(NdotV, ao, roughness);

	// Result should be in [0, 1] range (saturated)
	ASSERT(IsTrue, specAO >= 0.0 && specAO <= 1.0);

	// Should be finite
	ASSERT(IsTrue, isfinite(specAO));
}

/// @tags color, ao, specular, roughness-variation
[numthreads(1, 1, 1)]
void TestSpecularAORoughnessVariation()
{
	float NdotV = 0.8;
	float ao = 0.5;

	// Test different roughness values
	float smoothSpec = Color::SpecularAOLagarde(NdotV, ao, 0.0);    // Smooth
	float roughSpec = Color::SpecularAOLagarde(NdotV, ao, 1.0);     // Rough

	// Both should be valid
	ASSERT(IsTrue, smoothSpec >= 0.0 && smoothSpec <= 1.0);
	ASSERT(IsTrue, roughSpec >= 0.0 && roughSpec <= 1.0);

	// Rougher surfaces typically have different AO response
	// Just verify both are valid for now
}

// ============================================================================
// GAMMA CORRECTION TESTS
// ============================================================================

/// @tags color, gamma, correction
[numthreads(1, 1, 1)]
void TestGammaToLinear()
{
	// Test gamma to linear conversion
	float gamma05 = 0.5;
	float linear = Color::GammaToLinear(gamma05);

	// pow(0.5, 1.6) ≈ 0.33
	ASSERT(IsTrue, linear < gamma05);  // Linear should be darker
	ASSERT(IsTrue, linear > 0.0 && linear < 1.0);
}

/// @tags color, gamma, roundtrip
[numthreads(1, 1, 1)]
void TestGammaRoundtrip()
{
	// Test gamma -> linear -> gamma roundtrip
	float original = 0.5;

	float linear = Color::GammaToLinear(original);
	float backToGamma = Color::LinearToGamma(linear);

	// Should approximately reconstruct (within tolerance for numerical error)
	ASSERT(IsTrue, abs(backToGamma - original) < 0.1);
}

/// @tags color, gamma, extremes
[numthreads(1, 1, 1)]
void TestGammaExtremes()
{
	// Test edge cases
	float black = 0.0;
	float white = 1.0;

	// Black should stay black
	ASSERT(AreEqual, Color::GammaToLinear(black), 0.0);
	ASSERT(AreEqual, Color::LinearToGamma(black), 0.0);

	// White should stay white
	ASSERT(AreEqual, Color::GammaToLinear(white), 1.0);
	ASSERT(AreEqual, Color::LinearToGamma(white), 1.0);
}

// ============================================================================
// VECTOR VERSIONS
// ============================================================================

/// @tags color, gamma, vector
[numthreads(1, 1, 1)]
void TestGammaVectorConversion()
{
	// Test vector versions of gamma conversion
	float3 gammaColor = float3(0.5, 0.6, 0.7);

	float3 linearColor = Color::GammaToLinearVector(gammaColor);

	// Should convert each channel
	ASSERT(IsTrue, all(linearColor < gammaColor));  // Linear is darker
	ASSERT(IsTrue, all(linearColor >= 0.0));
	ASSERT(IsTrue, all(linearColor <= 1.0));

	// Test roundtrip
	float3 backToGamma = Color::LinearToGammaVector(linearColor);
	ASSERT(IsTrue, distance(backToGamma, gammaColor) < 0.1);
}

/// @tags color, constants
[numthreads(1, 1, 1)]
void TestColorConstants()
{
	// Verify color constants are sensible
	ASSERT(IsTrue, Color::PBRLightingScale > 0.0 && Color::PBRLightingScale <= 1.0);
	ASSERT(IsTrue, Color::ReflectionNormalisationScale > 0.0 && Color::ReflectionNormalisationScale <= 1.0);
	ASSERT(IsTrue, Color::GammaCorrectionValue > 1.0);  // Typical gamma is 2.2
}
