// Stereo Sync + Blur - Combined bilateral stereo blend and depth-weighted
// blur for VR screen-space shadows. Runs as a single compute pass after the
// raymarch to both synchronize shadow data between eyes and smooth per-pixel
// noise.
//
// Based on: Shi, Billeter, Eisemann 2022, "Stereo-consistent screen-space
// ambient occlusion" https://eprints.whiterose.ac.uk/id/eprint/187713/

#include "Common/FoveatedMask.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#ifdef VR

// Match the C++ depth binding format for strict typing.
// TERRAIN_BLENDING ON  -> R32_FLOAT (no unorm). OFF -> R24_UNORM_X8_TYPELESS (unorm).
#	if defined(TERRAIN_BLENDING)
Texture2D<float> SrcDepthTexture : register(t0);
#	else
Texture2D<unorm float> SrcDepthTexture : register(t0);
#	endif
Texture2D<unorm float> SrcShadowTexture : register(t1);

RWTexture2D<unorm float> OutShadowTexture : register(u0);

cbuffer StereoSyncCB : register(b1)
{
	float2 FrameDim;
	float2 RcpFrameDim;
	float2 DispatchBase;
	float2 DispatchExtent;
	float4 FoveatedData0;  // x=centerScale, y=centerFeather, z=centerHorizontalScale, w=enabled
	float4 FoveatedCenterOffset;
};

static const float kDepthSigma = 0.01;          // Bilateral depth tolerance (NDC): surfaces within this range are considered the same and blended
static const float kMaxBlend = 1.0;             // Maximum stereo blend weight; reduce below 1.0 to soften the cross-eye contribution
static const float kEdgeDepthThreshold = 0.05;  // NDC depth difference above which a pixel is considered a depth discontinuity and excluded from stereo sync
static const int kEdgeMargin = 2;               // Neighbor offset (pixels) for destination edge + mask boundary check

// Depth-weighted 4-sample blur using a rotated Poisson disk.
// Uses dtid hash for per-pixel rotation to break structured patterns.
float BlurShadow(int2 dtid, float centerDepth)
{
	// Per-pixel rotation from interleaved gradient noise
	float noise = Random::InterleavedGradientNoise(float2(dtid));
	float angle = noise * Math::TAU;
	float sn, cs;
	sincos(angle, sn, cs);
	float2x2 rot = float2x2(cs, sn, -sn, cs);

	static const float2 kOffsets[4] = {
		float2(0.382, 0.892),
		float2(0.491, 0.217),
		float2(0.938, 0.735),
		float2(0.009, 0.056),
	};

	float weight = 0;
	float shadow = 0;

	[unroll] for (uint i = 0; i < 4; i++)
	{
		float2 offset = mul(kOffsets[i], rot);
		int2 samplePx = dtid + int2(offset * 2.5);
		samplePx = clamp(samplePx, int2(0, 0), int2(FrameDim) - 1);

		float sampleDepth = SrcDepthTexture[samplePx];

		if (sampleDepth < 1e-5)
			continue;

		float attenuation = 1.0 - saturate(100.0 * abs(sampleDepth - centerDepth) / max(centerDepth, 1e-5));

		if (attenuation > 0.0) {
			shadow += SrcShadowTexture[samplePx] * attenuation;
			weight += attenuation;
		}
	}

	return weight > 0.0 ? shadow / weight : SrcShadowTexture[dtid];
}

// Samples four depth neighbors in a cross pattern (±offset pixels) around center,
// clamped to eyeIndex's half of the packed stereo buffer to avoid seam contamination.
float4 SampleCrossDepths(int2 center, int offset, uint eyeIndex)
{
	return float4(
		SrcDepthTexture[Stereo::ClampToEyeBounds(center + int2(offset, 0), eyeIndex, FrameDim)],
		SrcDepthTexture[Stereo::ClampToEyeBounds(center + int2(-offset, 0), eyeIndex, FrameDim)],
		SrcDepthTexture[Stereo::ClampToEyeBounds(center + int2(0, offset), eyeIndex, FrameDim)],
		SrcDepthTexture[Stereo::ClampToEyeBounds(center + int2(0, -offset), eyeIndex, FrameDim)]);
}

[numthreads(8, 8, 1)] void main(uint2 localID : SV_DispatchThreadID) {
	if (any(localID >= uint2(DispatchExtent)))
		return;

	uint2 dtid = localID + uint2(DispatchBase);
	if (any(dtid >= uint2(FrameDim)))
		return;

	float2 uv = (dtid + 0.5) * RcpFrameDim;

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	float centerWeight = 1.0;
	if (FoveatedData0.w > 0.5) {
		const float eyeWidth = max(FrameDim.x * 0.5, 1.0);
		float2 eyePx = float2(dtid);
		if (eyeIndex == 1)
			eyePx.x -= eyeWidth;

		const float2 eyeUV = saturate((eyePx + 0.5) / float2(eyeWidth, FrameDim.y));
		centerWeight = FoveatedComputeCenterBlendWeight(
			eyeUV,
			FoveatedData0.x,
			FoveatedData0.y,
			FoveatedData0.z,
			FoveatedCenterOffset.xy);
		if (centerWeight <= 0.0)
			return;
	}

	float depth = SrcDepthTexture[dtid];

	// depth == 0: VR HMD mask; depth == 1: sky/far plane
	if (depth < 1e-5 || depth >= 1.0) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	// Skip stereo sync for first-person geometry interior (hands/weapons).
	// Placed before the blur: arm shadow is uniform so the bilateral blur
	// would return SrcShadowTexture[dtid] unchanged anyway.
	float linearDepth = SharedData::GetScreenDepth(depth);
	if (linearDepth < VR_FP_Z) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	Stereo::StereoSyncParams params;
	params.edgeThreshold = kEdgeDepthThreshold;
	params.depthSigma = kDepthSigma;
	params.maxBlend = kMaxBlend;
	params.backCheckThreshold = 0.0;
	params.maskEpsilon = 1e-5;

	// Source-edge gate + reproject. On a source discontinuity (skipReason 1) the
	// bilateral depth weighting would collapse the blur to SrcShadowTexture[dtid]
	// anyway, so skip both sync and blur and pass through unmodified.
	float4 edgeDepths = SampleCrossDepths(dtid, 1, eyeIndex);
	Stereo::StereoBilateralResult r = Stereo::StereoSyncReproject(uv, depth, edgeDepths, eyeIndex, FrameDim, params);
	if (r.skipReason == 1) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	// Depth-weighted blur on this eye's shadow data.
	// Only reached by world pixels that will attempt stereo sync.
	float myShadow = BlurShadow(dtid, depth);

	// centerWeight fading already happened once in the source (bend raymarch);
	// re-fading here would compound it, so pass the blurred/blended value through.
	if (!r.valid) {
		OutShadowTexture[dtid] = myShadow;
		return;
	}

	float otherDepth = SrcDepthTexture[r.otherPx];

	// Skip if other eye sees mask, sky, or first-person geometry
	if (otherDepth < 1e-5 || otherDepth >= 1.0 || SharedData::GetScreenDepth(otherDepth) < VR_FP_Z) {
		OutShadowTexture[dtid] = myShadow;
		return;
	}

	// Destination-edge/mask gate + bilateral weight. The source-side edge check
	// only fires when *this* eye sees the boundary; due to VR parallax the arm
	// silhouette appears at a different screen position in each eye, so the
	// reprojection can cross a boundary invisible from this eye's perspective.
	float4 otherNeighbors = SampleCrossDepths(r.otherPx, kEdgeMargin, 1 - eyeIndex);
	Stereo::StereoSyncWeight(r, uv, depth, otherDepth, otherNeighbors, eyeIndex, FrameDim, params);

	float otherShadow = SrcShadowTexture[r.otherPx];

	// Use min (darkest) when depths agree: if either eye detected an
	// occluder, that shadow should be visible. blendWeight is 0 on a dest-edge
	// reject, collapsing the lerp to myShadow.
	float combined = min(myShadow, otherShadow);
	OutShadowTexture[dtid] = lerp(myShadow, combined, r.blendWeight);
}

#endif  // VR
