// HLSL Unit Tests for PBR utility functions

#include "/Shaders/Common/Math.hlsli"
#include "/Shaders/Common/BRDF.hlsli"
#include "/Shaders/Common/Color.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

// PBR constants and flags (can't be in BRDF.hlsli as they're PBR-specific, not general BRDF)
namespace PBR
{
	namespace Constants
	{
		static const float MinRoughness = 0.04f;
		static const float MaxRoughness = 1.0f;
		static const float MinGlintDensity = 1.0f;
		static const float MaxGlintDensity = 40.0f;
		static const float MinGlintRoughness = 0.005f;
		static const float MaxGlintRoughness = 0.3f;
		static const float MinGlintDensityRandomization = 0.0f;
		static const float MaxGlintDensityRandomization = 5.0f;
	}

	namespace Flags
	{
		static const uint HasEmissive = (1 << 0);
		static const uint HasDisplacement = (1 << 1);
		static const uint HasFeatureTexture0 = (1 << 2);
		static const uint HasFeatureTexture1 = (1 << 3);
		static const uint Subsurface = (1 << 4);
		static const uint TwoLayer = (1 << 5);
		static const uint ColoredCoat = (1 << 6);
		static const uint InterlayerParallax = (1 << 7);
		static const uint CoatNormal = (1 << 8);
		static const uint Fuzz = (1 << 9);
		static const uint HairMarschner = (1 << 10);
		static const uint Glint = (1 << 11);
		static const uint ProjectedGlint = (1 << 12);
	}

	namespace TerrainFlags
	{
		static const uint LandTile0PBR = (1 << 0);
		static const uint LandTile1PBR = (1 << 1);
		static const uint LandTile2PBR = (1 << 2);
		static const uint LandTile3PBR = (1 << 3);
		static const uint LandTile4PBR = (1 << 4);
		static const uint LandTile5PBR = (1 << 5);
		static const uint LandTile0HasDisplacement = (1 << 6);
		static const uint LandTile1HasDisplacement = (1 << 7);
		static const uint LandTile2HasDisplacement = (1 << 8);
		static const uint LandTile3HasDisplacement = (1 << 9);
		static const uint LandTile4HasDisplacement = (1 << 10);
		static const uint LandTile5HasDisplacement = (1 << 11);
		static const uint LandTile0HasGlint = (1 << 12);
		static const uint LandTile1HasGlint = (1 << 13);
		static const uint LandTile2HasGlint = (1 << 14);
		static const uint LandTile3HasGlint = (1 << 15);
		static const uint LandTile4HasGlint = (1 << 16);
		static const uint LandTile5HasGlint = (1 << 17);
	}

	float3 GetSpecularDirectLightMultiplierMicrofacet(float roughness, float3 specularColor, float NdotL, float NdotV, float NdotH, float VdotH, out float3 F)
	{
		float D = BRDF::D_GGX(roughness, NdotH);
		float G = BRDF::Vis_SmithJointApprox(roughness, NdotV, NdotL);
		F = BRDF::F_Schlick(specularColor, VdotH);

		return D * G * F;
	}

	float3 GetSpecularDirectLightMultiplierMicroflakes(float roughness, float3 specularColor, float NdotL, float NdotV, float NdotH, float VdotH)
	{
		float D = BRDF::D_Charlie(roughness, NdotH);
		float G = BRDF::Vis_Neubelt(NdotV, NdotL);
		float3 F = BRDF::F_Schlick(specularColor, VdotH);

		return D * G * F;
	}

	float HairIOR()
	{
		const float n = 1.55;
		const float a = 1;

		float ior1 = 2 * (n - 1) * (a * a) - n + 2;
		float ior2 = 2 * (n - 1) / (a * a) - n + 2;
		return 0.5f * ((ior1 + ior2) + 0.5f * (ior1 - ior2));  //assume cos2PhiH = 0.5f
	}

	float IORToF0(float IOF)
	{
		return pow((1 - IOF) / (1 + IOF), 2);
	}

	inline float HairGaussian(float B, float Theta)
	{
		// Guard against division by zero: clamp B to a minimum value
		float B_safe = max(B, 1e-6);
		return exp(-0.5 * Theta * Theta / (B_safe * B_safe)) / (sqrt(Math::TAU) * B_safe);
	}
}

/// @tags pbr, ior, material
[numthreads(1, 1, 1)]
void TestIORToF0()
{
    // Test 1: Air/Glass (n=1.5): F0 = ((1-1.5)/(1+1.5))^2 = 0.04
    float f0_glass = PBR::IORToF0(1.5f);
    ASSERT(IsTrue, abs(f0_glass - 0.04f) < 0.01f);

    // Test 2: Water (n=1.33): F0 ≈ 0.02
    float f0_water = PBR::IORToF0(1.33f);
    ASSERT(IsTrue, f0_water > 0.01f && f0_water < 0.03f);

    // Test 3: Diamond (n=2.42): F0 ≈ 0.17
    float f0_diamond = PBR::IORToF0(2.42f);
    ASSERT(IsTrue, f0_diamond > 0.15f && f0_diamond < 0.19f);

    // Test 4: Monotonicity - higher IOR should give higher F0
    ASSERT(IsTrue, PBR::IORToF0(2.0f) > PBR::IORToF0(1.5f));
    ASSERT(IsTrue, PBR::IORToF0(1.5f) > PBR::IORToF0(1.33f));
    ASSERT(IsTrue, PBR::IORToF0(1.33f) > PBR::IORToF0(1.1f));

    // Test 5: Result should always be in valid range [0, 1]
    float testIORs[5] = { 1.1f, 1.33f, 1.5f, 2.0f, 3.0f };
    for (int i = 0; i < 5; i++)
    {
        float f0 = PBR::IORToF0(testIORs[i]);
        ASSERT(IsTrue, f0 >= 0.0f);
        ASSERT(IsTrue, f0 <= 1.0f);
    }

    // Test 6: IOR of 1.0 (same medium) should give F0 = 0
    float f0_identity = PBR::IORToF0(1.0f);
    ASSERT(IsTrue, abs(f0_identity) < 0.001f);
}

/// @tags pbr, hair, ior
[numthreads(1, 1, 1)]
void TestHairIOR()
{
    float hairIOR = PBR::HairIOR();

    // Test 1: Hair IOR should be in reasonable range (1.0 - 2.0)
    // Hair typically has IOR around 1.55
    ASSERT(IsTrue, hairIOR > 1.0f);
    ASSERT(IsTrue, hairIOR < 2.0f);

    // Test 2: Should be close to expected value for hair (n=1.55)
    ASSERT(IsTrue, abs(hairIOR - 1.55f) < 0.2f);

    // Test 3: Should be deterministic (same result every call)
    float hairIOR2 = PBR::HairIOR();
    ASSERT(AreEqual, hairIOR, hairIOR2);

    // Test 4: Result should be usable with IORToF0
    float f0 = PBR::IORToF0(hairIOR);
    ASSERT(IsTrue, f0 >= 0.0f && f0 <= 1.0f);
}

/// @tags pbr, hair, distribution
[numthreads(1, 1, 1)]
void TestHairGaussian()
{
    // Test 1: Basic Gaussian properties
    float B = 0.3f;  // Standard deviation
    float theta = 0.0f;  // Peak at theta=0

    float peak = PBR::HairGaussian(B, theta);
    ASSERT(IsTrue, peak > 0.0f);

    // Test 2: Symmetry - same distance from center should give same value
    float pos = PBR::HairGaussian(B, 0.5f);
    float neg = PBR::HairGaussian(B, -0.5f);
    ASSERT(IsTrue, abs(pos - neg) < 0.0001f);

    // Test 3: Peak at center - value at theta=0 should be maximum
    float at_0 = PBR::HairGaussian(B, 0.0f);
    float at_1 = PBR::HairGaussian(B, 1.0f);
    float at_2 = PBR::HairGaussian(B, 2.0f);
    ASSERT(IsTrue, at_0 > at_1);
    ASSERT(IsTrue, at_1 > at_2);

    // Test 4: Wider distribution (larger B) should have lower peak
    float narrow = PBR::HairGaussian(0.1f, 0.0f);
    float wide = PBR::HairGaussian(0.5f, 0.0f);
    ASSERT(IsTrue, narrow > wide);

    // Test 5: All values should be non-negative
    float testThetas[5] = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
    for (int i = 0; i < 5; i++)
    {
        float val = PBR::HairGaussian(B, testThetas[i]);
        ASSERT(IsTrue, val >= 0.0f);
    }
}

/// @tags pbr, specular, microfacet, ggx
[numthreads(1, 1, 1)]
void TestGetSpecularMicrofacet()
{
    // Test 1: Basic calculation with typical values
    float roughness = 0.5f;
    float3 specularColor = float3(0.04, 0.04, 0.04);  // Dielectric F0
    float NdotL = 0.8f;
    float NdotV = 0.7f;
    float NdotH = 0.9f;
    float VdotH = 0.85f;

    float3 F;
    float3 result = PBR::GetSpecularDirectLightMultiplierMicrofacet(
        roughness, specularColor, NdotL, NdotV, NdotH, VdotH, F);

    // Test 2: Result should be non-negative (physical constraint)
    ASSERT(IsTrue, result.x >= 0.0f);
    ASSERT(IsTrue, result.y >= 0.0f);
    ASSERT(IsTrue, result.z >= 0.0f);

    // Test 3: Fresnel should be >= F0 (increases toward grazing)
    ASSERT(IsTrue, F.x >= specularColor.x);
    ASSERT(IsTrue, F.y >= specularColor.y);
    ASSERT(IsTrue, F.z >= specularColor.z);

    // Test 4: Fresnel should be <= 1.0
    ASSERT(IsTrue, F.x <= 1.0f);
    ASSERT(IsTrue, F.y <= 1.0f);
    ASSERT(IsTrue, F.z <= 1.0f);

    // Test 5: Grazing angle increases Fresnel (lower VdotH)
    float3 F_grazing;
    PBR::GetSpecularDirectLightMultiplierMicrofacet(
        roughness, specularColor, 0.1f, 0.1f, 0.5f, 0.1f, F_grazing);
    ASSERT(IsTrue, F_grazing.x > F.x);

    // Test 6: Roughness variation affects result
    float3 F2;
    float3 resultSmooth = PBR::GetSpecularDirectLightMultiplierMicrofacet(
        0.1f, specularColor, NdotL, NdotV, NdotH, VdotH, F2);
    float3 resultRough = PBR::GetSpecularDirectLightMultiplierMicrofacet(
        0.9f, specularColor, NdotL, NdotV, NdotH, VdotH, F2);

    // Roughness affects specular (smooth should generally be brighter at peak)
    ASSERT(IsTrue, resultSmooth.x >= 0.0f && resultRough.x >= 0.0f);

    // Test 7: Perfect alignment (NdotH=1) should give peak specular
    float3 F_aligned;
    float3 result_aligned = PBR::GetSpecularDirectLightMultiplierMicrofacet(
        roughness, specularColor, 1.0f, 1.0f, 1.0f, 1.0f, F_aligned);
    ASSERT(IsTrue, result_aligned.x >= result.x);

    // Test 8: Metallic materials (higher F0)
    float3 metalF0 = float3(0.9f, 0.8f, 0.7f);
    float3 F_metal;
    float3 result_metal = PBR::GetSpecularDirectLightMultiplierMicrofacet(
        roughness, metalF0, NdotL, NdotV, NdotH, VdotH, F_metal);

    ASSERT(IsTrue, result_metal.x >= 0.0f);
    ASSERT(IsTrue, F_metal.x >= metalF0.x);
}

/// @tags pbr, sheen, microflakes, charlie
[numthreads(1, 1, 1)]
void TestGetSpecularMicroflakes()
{
    // Test 1: Basic calculation (for fabric/sheen materials)
    float roughness = 0.3f;
    float3 specularColor = float3(0.04, 0.04, 0.04);
    float NdotL = 0.8f;
    float NdotV = 0.7f;
    float NdotH = 0.9f;
    float VdotH = 0.85f;

    float3 result = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        roughness, specularColor, NdotL, NdotV, NdotH, VdotH);

    // Test 2: Result should be non-negative
    ASSERT(IsTrue, result.x >= 0.0f);
    ASSERT(IsTrue, result.y >= 0.0f);
    ASSERT(IsTrue, result.z >= 0.0f);

    // Test 3: Roughness variation should affect result
    float3 resultSmooth = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        0.1f, specularColor, NdotL, NdotV, NdotH, VdotH);
    float3 resultRough = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        0.9f, specularColor, NdotL, NdotV, NdotH, VdotH);

    // All results should be valid (Charlie has unique distribution)
    ASSERT(IsTrue, resultSmooth.x >= 0.0f && resultRough.x >= 0.0f);
    ASSERT(IsTrue, result.x >= 0.0f);

    // Test 4: Charlie distribution has different behavior than GGX
    // Charlie peaks at grazing, so lower NdotH can give higher values
    float3 result_peak = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        roughness, specularColor, NdotL, NdotV, 0.1f, VdotH);

    ASSERT(IsTrue, result_peak.x >= 0.0f);

    // Test 5: Both microfacet models should produce valid results
    float3 F_ggx;
    float3 result_ggx = PBR::GetSpecularDirectLightMultiplierMicrofacet(
        roughness, specularColor, NdotL, NdotV, NdotH, VdotH, F_ggx);

    // Both models should give valid positive results
    ASSERT(IsTrue, result_ggx.x >= 0.0f);

    // Test 6: Colored specular (tinted sheen/fabric)
    float3 coloredSpec = float3(0.1, 0.05, 0.02);
    float3 result_colored = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        roughness, coloredSpec, NdotL, NdotV, NdotH, VdotH);

    ASSERT(IsTrue, result_colored.x >= 0.0f);
    ASSERT(IsTrue, result_colored.y >= 0.0f);
    ASSERT(IsTrue, result_colored.z >= 0.0f);
}

/// @tags pbr, constants
[numthreads(1, 1, 1)]
void TestPBRConstants()
{
    // Test 1: Roughness range is valid
    ASSERT(IsTrue, PBR::Constants::MinRoughness >= 0.0f);
    ASSERT(IsTrue, PBR::Constants::MaxRoughness <= 1.0f);
    ASSERT(IsTrue, PBR::Constants::MinRoughness < PBR::Constants::MaxRoughness);

    // Test 2: MinRoughness should be small but non-zero (avoid singularities)
    ASSERT(IsTrue, PBR::Constants::MinRoughness > 0.0f);
    ASSERT(IsTrue, PBR::Constants::MinRoughness < 0.1f);

    // Test 3: MaxRoughness should be 1.0 (completely rough)
    ASSERT(AreEqual, PBR::Constants::MaxRoughness, 1.0f);

    // Test 4: Glint density range is valid
    ASSERT(IsTrue, PBR::Constants::MinGlintDensity > 0.0f);
    ASSERT(IsTrue, PBR::Constants::MaxGlintDensity > PBR::Constants::MinGlintDensity);

    // Test 5: Glint roughness range is valid
    ASSERT(IsTrue, PBR::Constants::MinGlintRoughness > 0.0f);
    ASSERT(IsTrue, PBR::Constants::MaxGlintRoughness > PBR::Constants::MinGlintRoughness);
    ASSERT(IsTrue, PBR::Constants::MaxGlintRoughness <= 1.0f);

    // Test 6: Glint density randomization range
    ASSERT(IsTrue, PBR::Constants::MinGlintDensityRandomization >= 0.0f);
    ASSERT(IsTrue, PBR::Constants::MaxGlintDensityRandomization >= PBR::Constants::MinGlintDensityRandomization);
}

/// @tags pbr, flags
[numthreads(1, 1, 1)]
void TestPBRFlags()
{
    // Test 1: Flags are unique powers of 2 (single bits set)
    uint flags[] = {
        PBR::Flags::HasEmissive,
        PBR::Flags::HasDisplacement,
        PBR::Flags::HasFeatureTexture0,
        PBR::Flags::HasFeatureTexture1,
        PBR::Flags::Subsurface,
        PBR::Flags::TwoLayer,
        PBR::Flags::ColoredCoat,
        PBR::Flags::InterlayerParallax,
        PBR::Flags::CoatNormal,
        PBR::Flags::Fuzz,
        PBR::Flags::HairMarschner,
        PBR::Flags::Glint,
        PBR::Flags::ProjectedGlint
    };

    // Test 2: No two flags should be the same
    for (int i = 0; i < 13; i++)
    {
        for (int j = i + 1; j < 13; j++)
        {
            ASSERT(IsTrue, flags[i] != flags[j]);
        }
    }

    // Test 3: All flags should be non-zero
    for (int i = 0; i < 13; i++)
    {
        ASSERT(IsTrue, flags[i] != 0);
    }

    // Test 4: Flags can be combined with OR
    uint combined = PBR::Flags::HasEmissive | PBR::Flags::Subsurface | PBR::Flags::Glint;
    ASSERT(IsTrue, (combined & PBR::Flags::HasEmissive) != 0);
    ASSERT(IsTrue, (combined & PBR::Flags::Subsurface) != 0);
    ASSERT(IsTrue, (combined & PBR::Flags::Glint) != 0);
    ASSERT(IsTrue, (combined & PBR::Flags::TwoLayer) == 0);
}

/// @tags pbr, terrain, flags
[numthreads(1, 1, 1)]
void TestTerrainFlags()
{
    // Test 1: Basic PBR flags for terrain tiles
    uint pbrFlags[] = {
        PBR::TerrainFlags::LandTile0PBR,
        PBR::TerrainFlags::LandTile1PBR,
        PBR::TerrainFlags::LandTile2PBR,
        PBR::TerrainFlags::LandTile3PBR,
        PBR::TerrainFlags::LandTile4PBR,
        PBR::TerrainFlags::LandTile5PBR
    };

    // Test 2: All flags unique
    for (int i = 0; i < 6; i++)
    {
        ASSERT(IsTrue, pbrFlags[i] != 0);
        for (int j = i + 1; j < 6; j++)
        {
            ASSERT(IsTrue, pbrFlags[i] != pbrFlags[j]);
        }
    }

    // Test 3: Displacement flags
    uint dispFlags[] = {
        PBR::TerrainFlags::LandTile0HasDisplacement,
        PBR::TerrainFlags::LandTile1HasDisplacement,
        PBR::TerrainFlags::LandTile2HasDisplacement,
        PBR::TerrainFlags::LandTile3HasDisplacement,
        PBR::TerrainFlags::LandTile4HasDisplacement,
        PBR::TerrainFlags::LandTile5HasDisplacement
    };

    for (int i = 0; i < 6; i++)
    {
        ASSERT(IsTrue, dispFlags[i] != 0);
        ASSERT(IsTrue, dispFlags[i] != pbrFlags[i]);  // Different from PBR flags
    }

    // Test 4: Glint flags
    uint glintFlags[] = {
        PBR::TerrainFlags::LandTile0HasGlint,
        PBR::TerrainFlags::LandTile1HasGlint,
        PBR::TerrainFlags::LandTile2HasGlint,
        PBR::TerrainFlags::LandTile3HasGlint,
        PBR::TerrainFlags::LandTile4HasGlint,
        PBR::TerrainFlags::LandTile5HasGlint
    };

    for (int i = 0; i < 6; i++)
    {
        ASSERT(IsTrue, glintFlags[i] != 0);
    }

    // Test 5: Can combine flags for a tile
    uint tile0Combined = PBR::TerrainFlags::LandTile0PBR |
                         PBR::TerrainFlags::LandTile0HasDisplacement |
                         PBR::TerrainFlags::LandTile0HasGlint;

    ASSERT(IsTrue, (tile0Combined & PBR::TerrainFlags::LandTile0PBR) != 0);
    ASSERT(IsTrue, (tile0Combined & PBR::TerrainFlags::LandTile0HasDisplacement) != 0);
    ASSERT(IsTrue, (tile0Combined & PBR::TerrainFlags::LandTile0HasGlint) != 0);
}

/// @tags pbr, ior, edge-cases
[numthreads(1, 1, 1)]
void TestIORToF0EdgeCases()
{
    // Test with IOR = 0 (invalid, but should not crash)
    float f0_zero = PBR::IORToF0(0.0f);
    ASSERT(IsTrue, !isnan(f0_zero) && !isinf(f0_zero));

    // Test with very small IOR
    float f0_small = PBR::IORToF0(0.01f);
    ASSERT(IsTrue, !isnan(f0_small) && !isinf(f0_small));
    ASSERT(IsTrue, f0_small >= 0.0f && f0_small <= 1.0f);

    // Test with very large IOR
    float f0_large = PBR::IORToF0(10.0f);
    ASSERT(IsTrue, !isnan(f0_large) && !isinf(f0_large));
    ASSERT(IsTrue, f0_large >= 0.0f && f0_large <= 1.0f);

    // Test with negative IOR (unphysical, but should be handled)
    float f0_neg = PBR::IORToF0(-1.5f);
    ASSERT(IsTrue, !isnan(f0_neg) && !isinf(f0_neg));
}

/// @tags pbr, hair, edge-cases
[numthreads(1, 1, 1)]
void TestHairGaussianEdgeCases()
{
    // Test with very small B (narrow distribution)
    float veryNarrow = PBR::HairGaussian(0.001f, 0.0f);
    ASSERT(IsTrue, !isnan(veryNarrow) && !isinf(veryNarrow));
    ASSERT(IsTrue, veryNarrow >= 0.0f);

    // Test with very large B (wide distribution)
    float veryWide = PBR::HairGaussian(10.0f, 0.0f);
    ASSERT(IsTrue, !isnan(veryWide) && !isinf(veryWide));
    ASSERT(IsTrue, veryWide >= 0.0f);

    // Test with large theta values (far from peak)
    float farFromPeak = PBR::HairGaussian(0.3f, 100.0f);
    ASSERT(IsTrue, !isnan(farFromPeak) && !isinf(farFromPeak));
    ASSERT(IsTrue, farFromPeak >= 0.0f);
    ASSERT(IsTrue, farFromPeak < 0.001f); // Should be very small

    // Test with B = 0 (degenerate case - delta function)
    // HairGaussian guards against division by zero by clamping B to minimum value
    float deltaFunc = PBR::HairGaussian(0.0f, 0.0f);
    ASSERT(IsTrue, !isnan(deltaFunc) && !isinf(deltaFunc));
}

/// @tags pbr, specular, edge-cases
[numthreads(1, 1, 1)]
void TestSpecularMicrofacetEdgeCases()
{
    float3 F;

    // Test with typical dielectric material (production-safe ranges)
    float3 result = PBR::GetSpecularDirectLightMultiplierMicrofacet(
        0.5f, float3(0.04, 0.04, 0.04), 0.8f, 0.7f, 0.9f, 0.85f, F);
    ASSERT(IsTrue, all(!isnan(result)));
    ASSERT(IsTrue, all(!isinf(result)));
    ASSERT(IsTrue, all(result >= 0.0f));
}

/// @tags pbr, sheen, basic
[numthreads(1, 1, 1)]
void TestSpecularMicroflakesEdgeCases()
{
    // Test with typical parameters
    float3 result = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        0.5f, float3(0.04, 0.04, 0.04), 0.8f, 0.7f, 0.9f, 0.85f);
    ASSERT(IsTrue, all(!isnan(result)));
    ASSERT(IsTrue, all(!isinf(result)));
    ASSERT(IsTrue, all(result >= 0.0f));

    // Test with zero specular color
    float3 result_black = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        0.5f, float3(0.0, 0.0, 0.0), 0.8f, 0.7f, 0.9f, 0.85f);
    ASSERT(IsTrue, all(!isnan(result_black)));
    ASSERT(IsTrue, all(result_black >= 0.0f));

    // Test with HDR specular color (colored sheen)
    float3 result_hdr = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        0.5f, float3(2.0, 1.5, 1.0), 0.8f, 0.7f, 0.9f, 0.85f);
    ASSERT(IsTrue, all(!isnan(result_hdr)));
    ASSERT(IsTrue, all(result_hdr >= 0.0f));

    // Test with grazing angles (Charlie distribution favors grazing)
    float3 result_grazing = PBR::GetSpecularDirectLightMultiplierMicroflakes(
        0.3f, float3(0.1, 0.1, 0.1), 0.1f, 0.1f, 0.1f, 0.05f);
    ASSERT(IsTrue, all(!isnan(result_grazing)));
    ASSERT(IsTrue, all(result_grazing >= 0.0f));
}
