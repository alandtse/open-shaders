#ifndef __VR_STEREO_EFFECTS_DEPENDENCY_HLSL__
#define __VR_STEREO_EFFECTS_DEPENDENCY_HLSL__

#include "Common/FrameBuffer.hlsli"

#if defined(VR)
namespace VRStereoEffects
{
	/** Returns the selected eye origin in camera-relative world space. */
	float3 GetEyeOriginWorld(uint eyeIndex)
	{
		float4 eyeOrigin = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(0.0, 0.0, 0.0, 1.0));
		return eyeOrigin.xyz * rcp(eyeOrigin.w);
	}

	/** Clamps packed normalized SBS UVs to texel centers within the selected eye. */
	float2 ClampStereoUVToEyeTexel(float2 stereoUV, uint eyeIndex, float2 textureDimensions)
	{
		float2 halfTexel = 0.5 * rcp(textureDimensions);
		float2 minimumUV = float2(0.5 * eyeIndex + halfTexel.x, halfTexel.y);
		float2 maximumUV = float2(0.5 * (eyeIndex + 1) - halfTexel.x, 1.0 - halfTexel.y);
		return clamp(stereoUV, minimumUV, maximumUV);
	}

	/** Clamps dynamic-resolution-adjusted SBS UVs using the full packed target dimensions. */
	float2 ClampDynamicStereoUVToEyeTexel(float2 dynamicStereoUV, uint eyeIndex, float2 textureDimensions, float2 resolutionScale)
	{
		float2 halfTexel = 0.5 * rcp(textureDimensions);
		float2 minimumUV = float2(0.5 * resolutionScale.x * eyeIndex + halfTexel.x, halfTexel.y);
		float2 maximumUV = float2(0.5 * resolutionScale.x * (eyeIndex + 1) - halfTexel.x, resolutionScale.y - halfTexel.y);
		return clamp(dynamicStereoUV, minimumUV, maximumUV);
	}
}
#endif

#endif
