#ifndef NATIVE_SNOW_HLSLI
#define NATIVE_SNOW_HLSLI

namespace NativePBR
{
	static const float SnowCrystalScatter = 0.22f;
	static const float3 SnowUltravioletTint = float3(0.78f, 0.86f, 1.0f);

	uint SnowHash(uint2 cell)
	{
		uint value = cell.x * 1664525u + cell.y * 1013904223u;
		value ^= value >> 16u;
		value *= 2246822519u;
		value ^= value >> 13u;
		value *= 3266489917u;
		return value ^ (value >> 16u);
	}

	float SnowRandom(int2 cell, uint seed)
	{
		return float(SnowHash(asuint(cell) ^ uint2(seed, seed * 747796405u)) & 0x00ffffffu) *
			(1.0f / 16777216.0f);
	}

	float SnowNoise(float2 position, uint seed)
	{
		int2 cell = int2(floor(position));
		float2 blend = frac(position);
		blend = blend * blend * (3.0f - 2.0f * blend);
		float bottom = lerp(SnowRandom(cell, seed), SnowRandom(cell + int2(1, 0), seed), blend.x);
		float top = lerp(SnowRandom(cell + int2(0, 1), seed), SnowRandom(cell + int2(1, 1), seed), blend.x);
		return lerp(bottom, top, blend.y);
	}

	void SnowDetail(float2 hitUV, uint hitSeed, float4 tuning,
		out float variation, out float crystal)
	{
		const float2 rayCell = clamp(hitUV, -4096.0f, 4096.0f) * max(tuning.y, 1.0f);
		variation = (SnowNoise(rayCell, hitSeed) - 0.5f) * saturate(tuning.x);
		const int2 crystalCell = int2(floor(rayCell * 8.0f));
		const float inclusion = SnowRandom(crystalCell, hitSeed ^ 0x9e3779b9u);
		crystal = inclusion < saturate(tuning.w) ? saturate(tuning.z) : 0.0f;
	}

	float2 SnowLumpSlope(float2 hitUV, uint hitSeed, float rayFrequency, float lumpSize, float lumpDensity)
	{
		if (lumpDensity <= 0.0f)
			return 0.0f;
		const float size = max(lumpSize, 0.5f);
		const float2 position = clamp(hitUV, -4096.0f, 4096.0f) * max(rayFrequency, 1.0f) / size;
		const int2 cell = int2(floor(position));
		const float2 fraction = frac(position);
		const float2 blend = fraction * fraction * (3.0f - 2.0f * fraction);
		const float2 blendDerivative = 6.0f * fraction * (1.0f - fraction);
		const uint seed = hitSeed ^ 0x85ebca6bu;
		const float a = SnowRandom(cell, seed);
		const float b = SnowRandom(cell + int2(1, 0), seed);
		const float c = SnowRandom(cell + int2(0, 1), seed);
		const float d = SnowRandom(cell + int2(1, 1), seed);
		const float bottom = lerp(a, b, blend.x);
		const float top = lerp(c, d, blend.x);
		const float value = lerp(bottom, top, blend.y);
		const float density = saturate(lumpDensity);
		const float ramp = saturate((value - (1.0f - density)) / density);
		const float profileDerivative = 6.0f * ramp * (1.0f - ramp) / density;
		const float2 noiseSlope = float2(lerp(b - a, d - c, blend.y) * blendDerivative.x,
			(top - bottom) * blendDerivative.y);
		return clamp(noiseSlope * (profileDerivative / size), -2.0f, 2.0f);
	}
}

#endif
