#ifndef NATIVE_SNOW_HLSLI
#define NATIVE_SNOW_HLSLI

namespace NativePBR
{
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
		const float2 crystalCell = rayCell * 8.0f;
		const int2 cell = int2(floor(crystalCell));
		const float2 center = frac(crystalCell) - 0.5f;
		const float inclusion = SnowRandom(cell, hitSeed ^ 0x9e3779b9u) < saturate(tuning.w) ? 1.0f : 0.0f;
		crystal = inclusion * (1.0f - smoothstep(0.006f, 0.05f, dot(center, center))) * saturate(tuning.z);
	}
}

#endif
