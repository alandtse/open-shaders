#ifndef STEREO_EYE_HLSLI
#define STEREO_EYE_HLSLI

#include "Common/VR.hlsli"

// VR renders both eyes side by side in one target. Any pass that depends on screen position or
// neighbouring pixels (centre, radius, blur taps, convolution) must work in eye-local UVs.

#ifdef VR
static const uint kEyeCount = 2;
#else
static const uint kEyeCount = 1;
#endif

/// Eye-local UV in [0,1] for a texel of a buffer that holds kEyeCount eyes side by side.
float2 EyeLocalUV(uint2 pixel, float2 bufferSize, out uint eye)
{
	eye = Stereo::GetEyeIndexFromPixel(pixel, uint2(bufferSize));
	return Stereo::ConvertFromStereoUV((pixel + 0.5f) / bufferSize, eye);
}

/// Clamps a stereo UV to texel centres inside its eye so bilinear taps never blend in the other eye.
float2 ClampToEyeTexels(Texture2D<float4> tex, float2 stereoUV, uint eye)
{
	uint2 size;
	tex.GetDimensions(size.x, size.y);
	return Stereo::ClampToEyeUV(stereoUV, eye, size);
}

/// Samples an eye-local UV; outside [0,1] returns black so the other eye never leaks in.
float4 SampleEye(Texture2D<float4> tex, SamplerState samp, float2 localUV, uint eye)
{
	if (any(localUV < 0.0f) || any(localUV > 1.0f))
		return 0;
	return tex.SampleLevel(samp, ClampToEyeTexels(tex, Stereo::ConvertToStereoUV(localUV, eye), eye), 0);
}

/// Samples an eye-local UV with x clamped to the eye's edge texels; use for blur taps.
float4 SampleEyeClamped(Texture2D<float4> tex, SamplerState samp, float2 localUV, uint eye)
{
	return tex.SampleLevel(samp, ClampToEyeTexels(tex, Stereo::ConvertToStereoUV(localUV, eye), eye), 0);
}

#endif
