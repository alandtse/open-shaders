#include "PostProcessing/fullscreen.hlsli"

#include "Common/VR.hlsli"

cbuffer CameraCB : register(b1)
{
	float4 CameraScreen;
	float4 CameraLens;
	float4 CameraAberration;
	float4 CameraSoftness;
	float4 CameraGrain;
}

Texture2D<float4> InputTexture : register(t0);
SamplerState ColorSampler : register(s0);

#ifdef VR
static const float CameraEyeTexelScale = 2.0;
#else
static const float CameraEyeTexelScale = 1.0;
#endif

float2 CameraAspectScale()
{
	return float2(CameraLens.y, 1.0);
}

// Eye-local UV; in VR the packed target holds both eyes side by side.
float2 CameraEyeUV(float2 stereoUV, out uint eyeIndex)
{
	eyeIndex = Stereo::GetEyeIndexFromTexCoord(stereoUV);
	return Stereo::ConvertFromStereoUV(stereoUV, eyeIndex);
}

float CameraNormalizedRadius(float2 uv)
{
	return length((uv - 0.5) * CameraAspectScale()) / CameraLens.z;
}
