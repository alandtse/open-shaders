// MenuBGBlitPS.hlsl — DLSSperf main-menu / loading-screen BG blit.
// Fullscreen 1:1 sample of the source texture into kTOTAL/kMENUBG. The
// caller (MaybeBlitMenuBG) feeds DLSS-reconstructed testTexture (R16G16
// B16A16_FLOAT, displayRes) and the destination kTOTAL is R8G8B8A8_UNORM
// at the same dims — CopyResource can't do this because the formats
// differ, so a draw-based blit handles the implicit float→unorm
// conversion via the RTV format.
//
// Reuses UpscaleVS.hlsl for the fullscreen triangle and a linear clamp
// sampler. saturate() on UV is defense-in-depth for callers binding non-
// clamp samplers.

#include "Upscaling/UpscaleVS.hlsl"

#if defined(PSHADER)

typedef VS_OUTPUT PS_INPUT;

SamplerState LinearSampler : register(s0);
Texture2D<float4> SourceTex : register(t0);

float4 main(PS_INPUT input) :
	SV_Target
{
	return SourceTex.SampleLevel(LinearSampler, saturate(input.TexCoord), 0);
}

#endif
