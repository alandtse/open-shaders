// HLSL unit tests for the Bloom contribution applied during display mapping.

// Stubs for dependencies from ISHDR.hlsl (not exercised by these tests).
float3 GetTonemapFactorHejlBurgessDawson(float3 x, bool isHDR) { return x; }
static const float4 Param = { 0, 0, 0, 0 };

#include "/Shaders/Common/DisplayMapping.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags bloom
/// A disabled enhancement must retain the exact native Bloom expression.
[numthreads(1, 1, 1)] void TestBloomDisabledPreservesVanilla() {
	float3 mappedColor = float3(0.8, 0.4, 0.1);
	float3 vanillaBloom = float3(0.2, 0.3, 0.4);
	float3 enhancedBloom = float3(3.0, 2.0, 1.0);
	float3 vanillaMask = saturate(0.5 - mappedColor);

	float3 expected = mappedColor + vanillaMask * vanillaBloom;
	float3 actual = DisplayMapping::ApplyBloom(mappedColor, vanillaBloom, enhancedBloom, vanillaMask, false);

	ASSERT(IsTrue, all(actual == expected));
}

	/// @tags bloom, daytime
	/// Enhancement remains visible when the native daytime mask has closed.
	[numthreads(1, 1, 1)] void TestBloomEnhancementHasDaytimeHeadroom()
{
	float3 mappedColor = 0.8.xxx;
	float3 bloom = 0.5.xxx;
	float3 vanillaMask = saturate(0.3 - mappedColor);

	float3 vanillaOnly = DisplayMapping::ApplyBloom(mappedColor, bloom, bloom, vanillaMask, false);
	float3 enhanced = DisplayMapping::ApplyBloom(mappedColor, bloom, bloom, vanillaMask, true);

	ASSERT(IsTrue, all(vanillaOnly == mappedColor));
	ASSERT(IsTrue, all(enhanced > vanillaOnly));
	ASSERT(IsTrue, all(abs(enhanced - 0.9.xxx) < 0.000001));
}

/// @tags bloom
/// The enhancement path uses the processed signal rather than vanilla Bloom.
[numthreads(1, 1, 1)] void TestBloomUsesEnhancedSignal() {
	float3 mappedColor = 0.4.xxx;
	float3 vanillaBloom = 0.2.xxx;
	float3 enhancedBloom = 0.6.xxx;
	float3 vanillaMask = 0.1.xxx;

	float3 actual = DisplayMapping::ApplyBloom(mappedColor, vanillaBloom, enhancedBloom, vanillaMask, true);

	ASSERT(IsTrue, all(abs(actual - 0.76.xxx) < 0.000001));
}

	/// @tags bloom
	/// The enhancement path cannot add light without an input Bloom signal.
	[numthreads(1, 1, 1)] void TestBloomZeroSignalRemainsZero()
{
	float3 mappedColor = float3(0.9, 0.6, 0.3);
	float3 actual = DisplayMapping::ApplyBloom(mappedColor, 0.0.xxx, 0.0.xxx, 0.0.xxx, true);

	ASSERT(IsTrue, all(actual == mappedColor));
}

/// @tags bloom
/// Enhancement must not reduce extra mask headroom supplied by the weather.
[numthreads(1, 1, 1)] void TestBloomPreservesHigherNativeMask() {
	float3 mappedColor = 0.25.xxx;
	float3 bloom = float3(0.2, 0.4, 0.6);
	float3 vanillaMask = 1.0.xxx;

	float3 vanillaResult = DisplayMapping::ApplyBloom(mappedColor, bloom, bloom, vanillaMask, false);
	float3 enhancedResult = DisplayMapping::ApplyBloom(mappedColor, bloom, bloom, vanillaMask, true);

	ASSERT(IsTrue, all(enhancedResult == vanillaResult));
}
