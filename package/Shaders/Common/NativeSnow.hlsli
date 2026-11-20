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

	float SnowRandom(int2 cell)
	{
		return float(SnowHash(asuint(cell)) & 0x00ffffffu) * (1.0f / 16777216.0f);
	}

	float SnowNoise(float2 position)
	{
		int2 cell = int2(floor(position));
		float2 blend = frac(position);
		blend = blend * blend * (3.0f - 2.0f * blend);
		float bottom = lerp(SnowRandom(cell), SnowRandom(cell + int2(1, 0)), blend.x);
		float top = lerp(SnowRandom(cell + int2(0, 1)), SnowRandom(cell + int2(1, 1)), blend.x);
		return lerp(bottom, top, blend.y);
	}

	void SnowDetail(float3 worldPosition, float footprint, float4 tuning,
		out float variation, out float crystal)
	{
		const float scale = max(tuning.y, 8.0f);
		const float microScale = max(scale * 0.19f, 4.0f);
		const float microVisibility = saturate(1.0f - footprint / microScale);
		const float macro = SnowNoise(worldPosition.xy / scale);
		const float micro = SnowNoise(worldPosition.xy / microScale);
		variation = (lerp(macro, micro, 0.35f * microVisibility) - 0.5f) * saturate(tuning.x);
		const float crystalSpacing = max(scale * 0.11f, 4.0f);
		const float2 location = worldPosition.xy / crystalSpacing;
		const int2 cell = int2(floor(location));
		const float radius = length(frac(location) - 0.5f);
		const float inclusion = SnowRandom(cell + int2(137, 271)) < saturate(tuning.w) ? 1.0f : 0.0f;
		const float visibility = saturate(1.0f - footprint / (crystalSpacing * 0.45f));
		crystal = inclusion * (1.0f - smoothstep(0.04f, 0.17f, radius)) * visibility * saturate(tuning.z);
	}
}

#endif
