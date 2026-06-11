// VR Stereo Optimizations - Depth Fill Pixel Shader
//
// Stencil-culled Eye 1 pixels never get geometry depth written during the deferred
// pass (the stencil test kills the whole fragment, depth write included), so later
// depth-tested passes (water, sky, transparency) see holes and draw through geometry.
// This pass restores depth for exactly those pixels: the DSS stencil test (EQUAL,
// ref=1) hardware-masks the fullscreen triangle to culled pixels, and SV_Depth
// writes the same depth source the classification CS used when it marked the pixel
// reprojectable (TerrainBlending blended depth or the post-z-prepass copy).

Texture2D<float> SceneDepthTexture : register(t0);

struct PS_INPUT
{
	float4 Position: SV_Position;
	float2 TexCoord: TEXCOORD0;
};

// Force the stencil EQUAL=1 test before the shader so the PS only runs on the ~80% of
// pixels that were culled (MODE_MAIN), not the whole Eye 1 half. The depth write is still
// late (SV_Depth), but the early stencil cull removes the non-reprojected pixels' shading.
[earlydepthstencil] float main(PS_INPUT input) : SV_Depth
{
	// Depth source is full SBS resolution - SV_Position maps directly
	// (viewport is the Eye 1 half, so Position.x starts at eyeWidth).
	return SceneDepthTexture[int2(input.Position.xy)];
}
