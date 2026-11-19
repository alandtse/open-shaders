// Gap closing, first half: every texel takes the nearest depth around it, so shadows grow by Radius texels
// and slivers of light narrower than about twice that vanish.
cbuffer ShadowGapsCB : register(b0)
{
	uint2 Size;
	uint Slice;
	int Radius;
};

#if defined(SHADOW_ARRAY)
Texture2DArray<float> ShadowDepth : register(t0);
#else
Texture2D<float> ShadowDepth : register(t0);
#endif
RWTexture2D<float> Grown : register(u0);

float Load(int2 texel)
{
	texel = clamp(texel, 0, int2(Size) - 1);
#if defined(SHADOW_ARRAY)
	return ShadowDepth.Load(int4(texel, Slice, 0));
#else
	return ShadowDepth.Load(int3(texel, 0));
#endif
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Size))
		return;
	float nearest = 1.0;
	for (int y = -Radius; y <= Radius; ++y)
		for (int x = -Radius; x <= Radius; ++x)
			nearest = min(nearest, Load(int2(id.xy) + int2(x, y)));
	Grown[id.xy] = nearest;
}
