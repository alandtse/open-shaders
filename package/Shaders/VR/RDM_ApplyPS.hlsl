// Radial Density Mask Application Shader
// Based on VRPerfKit (fholger/vrperfkit) radial_density_mask.frag.hlsl - MIT License
// Uses discard + transparent output instead of depth writes

Texture2D<uint> MaskTexture : register(t0);
Texture2D<float4> SourceColor : register(t1);
SamplerState PointSampler : register(s0);

float4 main(float4 pos : SV_Position) : SV_TARGET
{
	// Load mask value:
	// 0 = Keep All (Full Quality) - discard, don't modify
	// 1 = Keep 1/2 (Half Quality) - discard kept pixels, cull others
	// 2 = Keep 1/4 (Quarter Quality) - discard kept pixels, cull others
	// 3 = Keep 1/16 (Edge Transition) - discard kept pixels, cull others
	// 4 = Cull All - return black/transparent

	uint mask = MaskTexture.Load(int3(pos.xy, 0));
	uint2 p = uint2(pos.xy);
	uint2 pHalf = p >> 1u; // Divide by 2

	// VRPerfKit approach: discard pixels we want to KEEP, return transparent for pixels we want to CULL

	// Zone 0: Inner - Keep All (Full Quality)
	if (mask == 0) discard;

	// Zone 1: Inner Ring - Half Resolution (1x2 pattern)
	// Keep pixels where checkerboard pattern matches
	if (mask == 1) {
		if ((pHalf.x & 0x01u) == (pHalf.y & 0x01u))
			discard; // Keep this pixel
		// Otherwise fall through to cull
	}

	// Zone 2: Middle Ring - Quarter Resolution (2x2 pattern)
	// Keep only pixels where both halfX and halfY are odd
	else if (mask == 2) {
		if (!((pHalf.x & 0x01u) != 0u || (pHalf.y & 0x01u) != 0u))
			discard; // Keep this pixel
		// Otherwise fall through to cull
	}

	// Zone 3: Edge Transition - 1/16th Resolution (4x4 block pattern)
	// Keep only top-left of each 4x4 block
	else if (mask == 3) {
		uint2 block = p & 0x03u;
		if (!((block.x & 0x03u) != 0u || (block.y & 0x03u) != 0u))
			discard; // Keep this pixel
		// Otherwise fall through to cull
	}

	// Zone 4 or any culled pixel: Return transparent black
	// This creates the "hole" that reconstruction/blur can fill
	return float4(0, 0, 0, 0);
}
