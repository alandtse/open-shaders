#include "ScreenSpaceReflections/common.hlsli"
Texture2D<float> depth : register(t0);
RWTexture2D<float> outDepth : register(u0);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	uint depth_width, depth_height;
	depth.GetDimensions(depth_width, depth_height);

	uint out_depth_width, out_depth_height;
	outDepth.GetDimensions(out_depth_width, out_depth_height);

	// The full-sized texture has floor-sized physical mips; the active rect uses ceil-sized cells.
	const uint mip = firstbithigh((uint)max(TexDim.x, TexDim.y)) - firstbithigh(max(depth_width, depth_height));
	const uint scale = 1u << mip;
	const uint2 inExtent = min(uint2(depth_width, depth_height), max(uint2(1, 1), (uint2(FrameDim) + scale - 1u) / scale));
	const uint2 outExtent = min(uint2(out_depth_width, out_depth_height), max(uint2(1, 1), (uint2(FrameDim) + 2u * scale - 1u) / (2u * scale)));
	const uint2 maxCoord = inExtent - 1u;

	uint2 vReadCoord = DTid.xy << 1;
	uint2 vWriteCoord = DTid.xy;

	if (any(vWriteCoord >= outExtent))
		return;

	float4 depth_samples = float4(
		depth[min(vReadCoord, maxCoord)].x,
		depth[min(vReadCoord + uint2(1, 0), maxCoord)].x,
		depth[min(vReadCoord + uint2(0, 1), maxCoord)].x,
		depth[min(vReadCoord + uint2(1, 1), maxCoord)].x);

	float min_depth = min(depth_samples.x, min(depth_samples.y, min(depth_samples.z, depth_samples.w)));

	bool needExtraSampleX = vWriteCoord.x == outExtent.x - 1u && vReadCoord.x + 2u < inExtent.x;
	bool needExtraSampleY = vWriteCoord.y == outExtent.y - 1u && vReadCoord.y + 2u < inExtent.y;

	if (needExtraSampleX) {
		min_depth = min(min_depth, min(depth[min(vReadCoord + uint2(2, 0), maxCoord)].x, depth[min(vReadCoord + uint2(2, 1), maxCoord)].x));
	}

	if (needExtraSampleY) {
		min_depth = min(min_depth, min(depth[min(vReadCoord + uint2(0, 2), maxCoord)].x, depth[min(vReadCoord + uint2(1, 2), maxCoord)].x));
	}

	if (needExtraSampleX && needExtraSampleY) {
		min_depth = min(min_depth, depth[min(vReadCoord + uint2(2, 2), maxCoord)].x);
	}

	outDepth[vWriteCoord] = min_depth;
}
