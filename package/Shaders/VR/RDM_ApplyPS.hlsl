Texture2D<uint> MaskTexture : register(t0);

void main(float4 pos : SV_Position)
{
	// Load mask value (0 = Keep, 1 = Cull/Mask)
	uint mask = MaskTexture.Load(int3(pos.xy, 0));

	// If mask is 0 (Center), discard this pixel (don't touch stencil).
	// If mask is 1 (Periphery), keep this pixel (write to stencil).
	if (mask == 0) discard;
}
