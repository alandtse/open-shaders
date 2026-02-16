/**
 * @file DepthUpscalePS.hlsl
 * @brief Conservative depth buffer upscaling for VR depth-based culling
 *
 * When upscaling (FSR/DLSS) is active, the depth buffer is rendered at a lower
 * resolution than the display. Skyrim VR's depth-based culling (OBBOcclusionTesting)
 * reads from the depth buffer to determine object visibility, but with a mismatched
 * resolution, objects may be incorrectly culled (appearing to flicker in/out of view).
 *
 * This shader upscales the low-resolution depth buffer to full resolution using a
 * conservative approach: minimum depth from a 2x2 neighborhood blended with point-
 * sampled depth. Near the HMD hidden area mask (depth == 0 in reversed-Z), point
 * sampling is used exclusively to prevent mask bleed from bilinear filtering.
 *
 * Based on depth upscaling approach by vrnord
 * https://github.com/vrnord/skyrim-community-shaders-VR-DLSS
 */

#include "Upscaling/UpscaleVS.hlsl"

#if defined(PSHADER)
#	include "Common/FrameBuffer.hlsli"
#	include "Common/SharedData.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float Depth : SV_Depth;
};

// Low-resolution depth buffer (copy of main depth at render resolution)
Texture2D<float> DepthLowRes : register(t0);

// Linear sampler for bilinear interpolation
SamplerState LinearSampler : register(s0);

cbuffer DepthUpscaleCB : register(b0)
{
	float2 SourceDim;    // Full texture dimensions (texels)
	float2 InvSourceDim; // 1.0 / SourceDim
	float2 Scale;        // resolutionScale (render/display ratio)
	float2 Pad;
};
/**
 * @brief Main pixel shader entry point
 *
 * Performs conservative depth upscaling by blending bilinear interpolation
 * (for smooth gradients) with minimum depth (for safe culling).
 */
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	// Map full-res UV to render-res UV (same transform as the engine's
	// GetDynamicResolutionAdjustedScreenPosition).
	float2 uv = Scale * input.TexCoord;

	// Per-eye clamping for SBS stereo: prevent sampling across the center seam.
	bool isRight = input.TexCoord.x >= 0.5;
	float halfScale = 0.5 * Scale.x;
	uv.x = clamp(uv.x, isRight ? halfScale : 0.0, isRight ? Scale.x : halfScale);
	uv.y = clamp(uv.y, 0.0, Scale.y);

	// Nearest texel coordinate for point sampling
	int2 texel = int2(floor(uv * SourceDim));

	// GatherRed fetches the 2x2 texel quad around the sample point.
	float4 depthQuad = DepthLowRes.GatherRed(LinearSampler, uv);
	float minDepth = min(min(depthQuad.x, depthQuad.y), min(depthQuad.z, depthQuad.w));

	// HMD hidden area mask: depth == 0 in reversed-Z.
	// If ANY sample in the 2x2 quad is 0, we're at or near the mask boundary.
	// Use point sampling only to avoid bilinear blending with mask pixels.
	if (minDepth == 0.0) {
		psout.Depth = DepthLowRes.Load(int3(texel, 0));
		return psout;
	}

	// All four neighbors are valid depth. Blend point-sampled depth toward
	// the conservative minimum for safe culling.
	float pointDepth = DepthLowRes.Load(int3(texel, 0));

	const float conservativeBias = 0.35;
	psout.Depth = lerp(pointDepth, minDepth, conservativeBias);

	return psout;
}

#endif