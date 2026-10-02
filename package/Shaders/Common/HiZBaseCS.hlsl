// Base level of a Hi-Z pyramid: one HIZ_DOWNSAMPLE_FACTOR^2 source tile per output texel.
// HIZ_SOURCE_UNORM selects a UNORM-class depth view; HIZ_REDUCTION_MIN a nearest-depth chain.

#include "Common/HiZReduction.hlsli"

#ifdef HIZ_SOURCE_UNORM
Texture2D<unorm float> SrcDepth : register(t0);
#else
Texture2D<float> SrcDepth : register(t0);
#endif

RWTexture2D<float> HiZBase : register(u0);

// Must match HiZPyramid::kDownsampleFactor.
#define HIZ_DOWNSAMPLE_FACTOR 4

cbuffer HiZParams : register(b0)
{
	uint2 SourceSize;    // full source extent; both eyes when this chain is eye-separated
	uint2 BaseSize;      // base-level texels this chain writes
	uint WindowOriginX;  // this chain's source window origin, in source texels
	uint WindowWidth;    // this chain's source window extent, in source texels
};

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (any(tid.xy >= BaseSize))
		return;

	const int2 tile = int2(tid.xy) * HIZ_DOWNSAMPLE_FACTOR;

	float d = HIZ_REDUCTION_IDENTITY;
	[unroll] for (int y = 0; y < HIZ_DOWNSAMPLE_FACTOR; ++y)
	{
		[unroll] for (int x = 0; x < HIZ_DOWNSAMPLE_FACTOR; ++x)
		{
			const int2 local = tile + int2(x, y);
			// Outside the window reads as far plane; the clamp keeps speculated loads out of the other eye.
			const int2 address = clamp(int2(WindowOriginX, 0) + local, int2(0, 0), int2((int)(WindowOriginX + WindowWidth), (int)SourceSize.y) - 1);
			const bool insideWindow = local.x < (int)WindowWidth && local.y < (int)SourceSize.y;
			d = HIZ_REDUCE2(d, insideWindow ? SrcDepth.Load(int3(address, 0)) : HiZFarDepth);
		}
	}

	HiZBase[tid.xy] = d;
}
