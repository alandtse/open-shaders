// MenuBGStretchPS.hlsl — DLSSperf main-menu / loading-screen BG stretch.
// Bilinear upscale of kMAIN (renderRes) into kTOTAL/kMENUBG (displayRes)
// so the menu background — drawn by the engine into the small kMAIN that
// the BSOpenVR size hook caused — survives to the OpenVR submit. Without
// this, kTOTAL gets only the menu UI compositor's output and the BG
// (Skyrim logo, mist sprites, loading screen art) is missing.
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
