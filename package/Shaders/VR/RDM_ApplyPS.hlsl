Texture2D<uint> MaskTexture : register(t0);

struct PS_OUTPUT
{
	float depth : SV_Depth;
};

PS_OUTPUT main(float4 pos : SV_Position)
{
	// Load mask value:
	// 0 = 1x1 (Keep All)
	// 1 = 1x2 (Keep 1/2)
	// 2 = 2x2 (Keep 1/4)
	// 3 = 4x4 (Cull All)

	uint mask = MaskTexture.Load(int3(pos.xy, 0));

	// Zone 0: Keep All -> Discard depth write
	if (mask == 0) discard;

	// Zone 3: Cull All (Outer Ring) -> Write Depth
	if (mask == 3) {
		PS_OUTPUT output;
		output.depth = 0.0f;
		return output;
	}

	// Zone 2: 2x2 (Middle Ring) -> Keep 1 out of 4 pixels
	if (mask == 2) {
		// Keep if (x%2==0 && y%2==0)
		uint2 p = uint2(pos.xy);
		if ((p.x & 1) == 0 && (p.y & 1) == 0) discard;
	}

	// Zone 1: 1x2 (Inner Ring) -> Keep 1 out of 2 pixels
	// Check favoring (how do we know favoring here? Pass via constant buffer? Or assume based on pattern?)
	// Actually, we can just use a simple checkerboard or scanline.
	// Let's assume standard checkerboard (x+y)%2 for smoother look?
	// Or scanlines?
	// Let's use scanlines to match VRS 1x2 behavior approximately.
	if (mask == 1) {
		// Keep even Y rows (1x2 behavior - half vertical res)
		// Or even X cols (2x1 behavior)
		// Since we don't have the setting here, let's just do checkerboard for now?
		// No, checkerboard is 2x reduction but distributed.
		// Let's discard if (x+y)%2 != 0
		uint2 p = uint2(pos.xy);
		if ((p.x + p.y) % 2 == 0) discard;
	}

	PS_OUTPUT output;
	output.depth = 0.0f;
	return output;
}
