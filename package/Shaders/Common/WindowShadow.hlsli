#ifndef WINDOW_SHADOW_HLSLI
#define WINDOW_SHADOW_HLSLI

namespace WindowShadow
{
	Texture2D<float> NearEntry : register(t66);
	Texture2D<float> FarEntry : register(t67);
	Texture2D<float> DistantEntry : register(t68);
	Texture2D<float> CasterEntry : register(t69);

	// Positive floats sort by depth under MAX blending. The low bit records whether the selected pane admits sun.
	float Encode(float depth, bool open)
	{
		float ordered = depth < 0 ? rcp(1.0 - depth) : 1.0 + depth;
		return asfloat((asuint(ordered) & ~1u) | uint(open));
	}

	bool Open(float entry)
	{
		return (asuint(entry) & 1u) != 0;
	}

	bool ReachesPane(float depth, float entry)
	{
		return Encode(depth, false) >= asfloat(asuint(entry) & ~1u);
	}

	void ClipSunwardCaster(float2 rasterPosition, float casterDepth)
	{
		float entry = CasterEntry.Load(int3(int2(rasterPosition.xy), 0));
		if (!ReachesPane(casterDepth, entry))
			discard;
	}

	float TexelVisibility(Texture2D<float> entries, int2 texel, float receiver)
	{
		float entry = entries.Load(int3(texel, 0));
		return Open(entry) && receiver >= asfloat(asuint(entry) & ~1u);
	}

	float ReceiverVisibility(Texture2D<float> entries, float3 positionLS, bool filtered)
	{
		uint width, height;
		entries.GetDimensions(width, height);
		if (width == 0)
			return 1;
		float2 texel = positionLS.xy * float2(width, height);
		float receiver = Encode(positionLS.z, false);
		if (!filtered)
			return TexelVisibility(entries, int2(floor(texel)), receiver);
		texel -= 0.5;
		int2 base = int2(floor(texel));
		float2 fraction = frac(texel);
		float4 weightX = float4(1.0 - fraction.x, 1, 1, fraction.x);
		float4 weightY = float4(1.0 - fraction.y, 1, 1, fraction.y);
		// A 3x3 PCF footprint has four bilinear contributions per axis.
		float visibility = 0;
		[unroll] for (int y = 0; y < 4; ++y)
			[unroll] for (int x = 0; x < 4; ++x)
				visibility += weightX[asuint(x)] * weightY[asuint(y)] * TexelVisibility(entries, base + int2(x - 1, y - 1), receiver);
		return visibility / 9.0;
	}

	float Visibility(uint cascade, float3 positionLS, bool filtered = true)
	{
		float visibility = 1.0;
		if (cascade == 0)
			visibility = ReceiverVisibility(NearEntry, positionLS, filtered);
		else if (cascade == 1)
			visibility = ReceiverVisibility(FarEntry, positionLS, filtered);
		else
			visibility = ReceiverVisibility(DistantEntry, positionLS, filtered);
		return visibility;
	}
}
#endif
