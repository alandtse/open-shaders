// VR Stereo Optimizations - Depth Fill Pixel Shader
//
// Stencil-culled Eye 1 pixels never get geometry depth written during the deferred
// pass (the stencil test kills the whole fragment, depth write included), so later
// depth-tested passes (water, sky, transparency) see holes and draw through geometry.
// This pass restores depth for exactly those pixels: the DSS stencil test (EQUAL,
// ref=1) hardware-masks the fullscreen triangle to culled pixels, and SV_Depth
// writes the depth the classification CS used when it marked the pixel reprojectable.
// That depth is z-prepass depth, which omits some opaque geometry (alpha-tested statics), so
// where Eye 0's final depth disagrees the pixel searches Eye 0's row for the surface it really
// sees. A row suffices because the eyes differ by translation only: epipolar lines are horizontal.

#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "VRStereoOptimizations/cbuffers.hlsli"

Texture2D<float> SceneDepthTexture : register(t0);  // classification depth source (full SBS)
Texture2D<float> Eye0DepthTexture : register(t1);   // Eye 0 final geometry depth (Eye 0 half width)

struct PS_INPUT
{
	float4 Position: SV_Position;
	float2 TexCoord: TEXCOORD0;
};

bool IsSkyDepth(float depth)
{
	return (depth < EPSILON_DEPTH_SKY) || (depth >= 1.0);
}

// Both tests are needed: raw depth is hyperbolic, so at distance a different surface can sit
// within the raw threshold, while up close it can sit within the linear one.
bool DepthsAgree(float a, float b)
{
	float rawDiff = abs(a - b) / max(max(a, b), EPSILON_DIVISION);
	float linA = SharedData::GetScreenDepth(a);
	float linB = SharedData::GetScreenDepth(b);
	float linDiff = abs(linA - linB) / max(max(linA, linB), EPSILON_DIVISION);
	return rawDiff <= DisocclusionThreshold && linDiff <= EdgeDepthThreshold;
}

// Never add [earlydepthstencil] here: forced early-Z writes the rasterized triangle depth
// (0 = unrendered) instead of SV_Depth, wiping the culled pixels' depth.
float main(PS_INPUT input) : SV_Depth
{
	// Depth source is full SBS resolution - SV_Position maps directly
	// (viewport is the Eye 1 half, so Position.x starts at eyeWidth).
	int2 px = int2(input.Position.xy);
	float depth = SceneDepthTexture[px];

	if (RepairSearchRadius == 0 || IsSkyDepth(depth))
		return depth;

	float2 uv = (float2(px) + 0.5) / FrameDim;
	Stereo::StereoBilateralResult r = Stereo::ReprojectToOtherEye(uv, depth, 1, FrameDim);
	if (!r.valid)
		return depth;

	float eye0Depth = Eye0DepthTexture[r.otherPx];
	if (DepthsAgree(depth, eye0Depth))
		return depth;

	// Only a surface nearer than the classification one can be the occluder the prepass
	// missed, and nearer surfaces shift the reprojection target in one direction only.
	float bestDepth = depth;
	float bestLinear = SharedData::GetScreenDepth(depth);
	Stereo::StereoBilateralResult nearer = Stereo::ReprojectToOtherEye(uv, depth * 0.99, 1, FrameDim);
	int step = (nearer.valid && nearer.otherPx.x < r.otherPx.x) ? -1 : 1;
	int2 row = r.otherPx;
	[loop] for (uint i = 1; i <= RepairSearchRadius; i++)
	{
		uint2 candidate = Stereo::ClampToEyeBounds(int2(row.x + step * (int)i, row.y), 0, FrameDim);
		float candidateDepth = Eye0DepthTexture[candidate];
		if (IsSkyDepth(candidateDepth))
			continue;

		float candidateLinear = SharedData::GetScreenDepth(candidateDepth);
		if (candidateLinear >= bestLinear)
			continue;

		// The candidate is what this pixel sees only if its own reprojection lands here.
		float2 candidateUV = (float2(candidate) + 0.5) / FrameDim;
		Stereo::StereoBilateralResult back = Stereo::ReprojectToOtherEye(candidateUV, candidateDepth, 0, FrameDim);
		if (!back.valid || any(abs(back.otherPx - px) > 1))
			continue;

		bestDepth = candidateDepth;
		bestLinear = candidateLinear;
	}

	return bestDepth;
}
