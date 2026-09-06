// VR Stereo Optimizations - Eye 0 Depth Copy Pixel Shader
//
// Writes Eye 0's final geometry depth into an R32 target the depth-fill pass reads while
// kMAIN's depth is bound as its DSV. A draw, not a copy: D3D11 rejects a partial
// CopySubresourceRegion of a depth-stencil resource (the copy is silently dropped).

Texture2D<float> DepthTexture : register(t0);  // scene depth (full SBS), Eye 0 half read

struct PS_INPUT
{
	float4 Position: SV_Position;
	float2 TexCoord: TEXCOORD0;
};

float main(PS_INPUT input) : SV_Target
{
	// Viewport is the Eye 0 half, so Position.xy maps directly into the SBS source.
	return DepthTexture[int2(input.Position.xy)];
}
