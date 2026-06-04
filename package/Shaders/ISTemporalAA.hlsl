#include "Common/Color.hlsli"
#include "Common/DisplayMapping.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/VR.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
	float4 Feedback: SV_Target1;
};

#if defined(PSHADER)

Texture2D<float4> currentFrameTex : register(t0);
Texture2D<float4> historyTex : register(t1);
Texture2D<float4> velocityTex : register(t2);
Texture2D<float4> depthTex : register(t3);
Texture2D<float4> maskTex : register(t4);
Texture2D<float4> alphaTex : register(t5);

SamplerState currentFrameSampler : register(s0);
SamplerState historySampler : register(s1);
SamplerState velocitySampler : register(s2);
SamplerState depthSampler : register(s3);
SamplerState maskSampler : register(s4);
SamplerState alphaSampler : register(s5);

cbuffer PerGeometry : register(b2)
{
	float4 TexelSizeParams : packoffset(c0);
	float4 JitterAndRes : packoffset(c1);
	float4 NeighborWeights : packoffset(c2);
	float4 TexelOffset : packoffset(c3);
	float4 BlendParams : packoffset(c4);
	float4 ThresholdParams : packoffset(c5);
};

// Decompiler comparison idiom: cmp(expr) => -(expr), used as a truthy mask in ?: selects.
#	define cmp -

#	ifdef HDR_OUTPUT
// Internal working space for TAA is PQ/BT2020.
// PQ maps [0, 10000 nits] to [0, 1], so the vanilla 1.001 bracket ceiling is correct —
// nothing in the scene legitimately exceeds 1.0 PQ. This is why PQ avoids the bracket
// collapse that caused halos with the linear BT2020 working space.
float3 ConvertRenderInput(float3 gammaColor)
{
	return DisplayMapping::LinearToPQ(Color::BT709ToBT2020(Color::GammaToLinearSafe(gammaColor)), 10000.0);
}
float3 ConvertRenderOutput(float3 pqColor)
{
	return Color::LinearToGammaSafe(Color::BT2020ToBT709(DisplayMapping::PQtoLinear(pqColor, 10000.0)));
}
// Feedback luma round-trip: feedbackOut.x is read back as history.x next frame.
// Storing raw PQ luma [0,1] in a low-precision RT causes quantization banding in highlights
// because PQ encodes high nit values in the upper portion of the [0,1] range where
// 8/10-bit steps are perceptible. Encoding as game-gamma spreads precision like SDR
// and round-trips cleanly through whatever precision the feedback RT uses.
float EncodeFeedbackLuma(float pqLuma)
{
	// PQ → linear (single channel: luma only, no colour transform needed)
	float linearLuma = DisplayMapping::PQtoLinear(pqLuma.xxx, 10000.0).x;
	return Color::LinearToGammaSafe(linearLuma);
}
float DecodeFeedbackLuma(float gammaLuma)
{
	float linearLuma = Color::GammaToLinearSafe(gammaLuma);
	return DisplayMapping::LinearToPQ(linearLuma.xxx, 10000.0).x;
}
#	endif

static const float3 kLumaWeights = float3(0.5, 0.25, 0.25);

// Named magic constants from the vanilla decompile (values unchanged).
static const float kMaxLumaCap = 1.00100005;              // bracket ceiling (just above 1.0 PQ)
static const float kMinLumaCap = -0.00100000005;          // bracket floor (just below 0)
static const float kLumaEpsilon = 0.00999999978;          // negligible-luma threshold
static const float2 kSimilarityScale = float2(20, 100);   // motion-diff convergence decay (x), strict (y)
static const float kHistoryLumaDecay = 0.949999988;       // 0.95 decay applied to history motion luma
static const float kHistoryBlendThreshold = 0.902499974;  // below this blend weight, clamp to neighbourhood
static const float kFeedbackBlendMax = 0.99000001;        // ~1.0 ceiling for the feedback blend weight
static const float kFlickerThreshold = 0.200000003;       // luma-spread below this counts as flicker

/*
 * Channel layout (vanilla decompile — swizzles are load-bearing):
 * - Neighbour taps: .yxz sample; luma via dot(.xzy, kLumaWeights); stored as float4(.xyz=GRB, .w=luma)
 *   in neighbors[0..6] (uvMin,A0,A1,B0,B1,C0,C1), with masks in neighborBelowHist[].
 * - centre: centerColor float3 = RGB; luma via dot(centerColor.yzx, kLumaWeights).
 * - corner: float4 .xyz=corner GRB, .w=corner luma (depth-guided).
 * - Bracket colours (float3): (R, B, luma) with .z = luma — see MergeLumaBracket/MergeMaxBracket.
 * - Output colour lives in .yzw (vanilla r3.yzw after blend; resolved/outPacked .yzw), not .xyz.
 *
 * The few remaining float4 packs (motionReject, corner, the neighbour taps) are semantic but reuse
 * components like the decompile; the blend-math and history packs have been split into named locals.
 */

float2 ClampScreenUV(float2 screenUV, float2 drMax)
{
	return min(max(FrameBuffer::DynamicResolutionParams1.xy * screenUV, float2(0, 0)), drMax);
}

float4 ClampScreenUV4(float4 screenUV, float2 drMax)
{
	return min(max(FrameBuffer::DynamicResolutionParams1.xyxy * screenUV, float4(0, 0, 0, 0)), drMax.xyxy);
}

float2 ClampHistoryUV(float2 reprojectedUV)
{
	float2 uv = max(FrameBuffer::DynamicResolutionParams1.zw * reprojectedUV, float2(0, 0));
	uv.x = min(FrameBuffer::DynamicResolutionParams2.w, uv.x);
	uv.y = min(FrameBuffer::DynamicResolutionParams1.w, uv.y);
	return uv;
}

float2 GetDynamicResolutionMax()
{
	return float2(FrameBuffer::DynamicResolutionParams2.z, FrameBuffer::DynamicResolutionParams1.y);
}

// Neighbour tap: .yxz sample; luma via dot(.xzy, kLumaWeights). See channel-layout comment above.
struct ISTAA_NeighborTap
{
	float3 grb;
	float luma;
	float belowHist;
};

float3 LoadNeighborGRB(float2 uv)
{
	float3 grb = currentFrameTex.Sample(currentFrameSampler, uv).yxz;
#	ifdef HDR_OUTPUT
	grb.yxz = ConvertRenderInput(grb.yxz);
#	endif
	return grb;
}

ISTAA_NeighborTap SampleNeighborGRB(float2 uv, float historyLuma)
{
	ISTAA_NeighborTap tap;
	tap.grb = LoadNeighborGRB(uv);
	tap.luma = dot(tap.grb.xzy, kLumaWeights);
	tap.belowHist = cmp(tap.luma < historyLuma);
	return tap;
}

float4 PackNeighborTap(ISTAA_NeighborTap tap)
{
	return float4(tap.grb, tap.luma);
}

// Centre tap: .xyz sample into .yzw layout; luma via dot(.zwy, kLumaWeights).
float3 SampleCenterRGB(float2 uv)
{
	float3 rgb = currentFrameTex.Sample(currentFrameSampler, uv).xyz;
#	ifdef HDR_OUTPUT
	rgb = ConvertRenderInput(rgb);
#	endif
	return rgb;
}

float AlphaCoverageMask(float2 uv)
{
	return cmp(0 < alphaTex.Sample(alphaSampler, uv).z);
}

float FlickerLumaContribution(float centerLuma, float neighborLuma)
{
	float d = centerLuma + -neighborLuma;
	d = kFlickerThreshold + -abs(d);
	return ceil(d);
}

// Fold one neighbour tap into the running min-luma colour bracket. Vanilla decompile pack:
// a tap is float4(GRB.xyz, luma.w); the bracket is a float3 (R, B, luma) whose .z is the luma.
// The lower-luma colour is committed unless `gate` (the tap's belowHist mask) is set — matching
// the decompile's cmp/select/select order exactly.
float3 MergeLumaBracket(float4 tap, float3 bracket, float gate)
{
	float3 cand = cmp(tap.w < bracket.z) ? tap.yzw : bracket;
	return gate ? bracket : cand;
}

// MAX-luma counterpart: commit the tap's colour when it is BRIGHTER than the running bracket.
// NOTE the gate is INVERTED vs MergeLumaBracket — the decompile's max pass commits the new pick
// when the belowHist gate is SET (gate ? cand : bracket), the opposite of the min pass. (Getting
// this backwards reads EQUIVALENT on a static menu but diverges under motion.)
float3 MergeMaxBracket(float4 tap, float3 bracket, float gate)
{
	float3 cand = cmp(bracket.z < tap.w) ? tap.yzw : bracket;
	return gate ? cand : bracket;
}

// RCAS-style sharpen delta: 2 * (center - 0.25*a - 0.25*b), in the decompile's exact op order.
float SharpenDelta(float center, float a, float b)
{
	float d = -a * 0.25 + center;
	d = -b * 0.25 + d;
	return d + d;
}

// shallowestDepth must already include depth before calling.
float2 PickIfShallowestUV(float2 selectedUV, float shallowestDepth, float depth, float2 uvIfMatch)
{
	return cmp(shallowestDepth == depth) ? uvIfMatch : selectedUV;
}

// Pick the shallowest-depth UV in the 3x3 neighbourhood (outputs clamped DR UV sets for later taps).
float2 SelectDepthGuidedUV(
	float2 texCoord,
	float2 drMax,
	out float2 drUVMin,
	out float2 drUVMax,
	out float2 drCenter,
	out float4 drNeighborsA,
	out float4 drNeighborsB,
	out float4 drNeighborsC,
	out float3 cornerColorGRB)
{
	float2 uvMin = -TexelOffset.xy + texCoord;
	float2 uvMax = TexelOffset.xy + texCoord;

	drUVMax = ClampScreenUV(uvMax, drMax);
	float depthMaxCorner = depthTex.Sample(depthSampler, drUVMax).x;
	cornerColorGRB = LoadNeighborGRB(drUVMax);

	float4 neighborsA = TexelOffset.xyxy * float4(1, -1, 1, 0) + texCoord.xyxy;
	drNeighborsA = ClampScreenUV4(neighborsA, drMax);
	float depthA0 = depthTex.Sample(depthSampler, drNeighborsA.xy).x;
	float shallowestDepth = min(depthA0, depthMaxCorner);

	drUVMin = ClampScreenUV(uvMin, drMax);
	float depthMinCorner = depthTex.Sample(depthSampler, drUVMin).x;
	shallowestDepth = min(depthMinCorner, shallowestDepth);

	float2 selectedUV = PickIfShallowestUV(uvMax, shallowestDepth, depthMinCorner, uvMin);
	selectedUV = PickIfShallowestUV(selectedUV, shallowestDepth, depthA0, neighborsA.xy);

	float4 neighborsB = TexelOffset.xyxy * float4(0, -1, -1, 1) + texCoord.xyxy;
	drNeighborsB = ClampScreenUV4(neighborsB, drMax);
	float depthB0 = depthTex.Sample(depthSampler, drNeighborsB.xy).x;
	shallowestDepth = min(depthB0, shallowestDepth);
	float depthA1 = depthTex.Sample(depthSampler, drNeighborsA.zw).x;
	shallowestDepth = min(depthA1, shallowestDepth);

	selectedUV = PickIfShallowestUV(selectedUV, shallowestDepth, depthA1, neighborsA.zw);
	selectedUV = PickIfShallowestUV(selectedUV, shallowestDepth, depthB0, neighborsB.xy);

	float4 neighborsC = TexelOffset.xyxy * float4(-1, 0, 0, 1) + texCoord.xyxy;
	drNeighborsC = ClampScreenUV4(neighborsC, drMax);
	float depthC0 = depthTex.Sample(depthSampler, drNeighborsC.xy).x;
	shallowestDepth = min(depthC0, shallowestDepth);
	float depthB1 = depthTex.Sample(depthSampler, drNeighborsB.zw).x;
	shallowestDepth = min(depthB1, shallowestDepth);

	selectedUV = PickIfShallowestUV(selectedUV, shallowestDepth, depthB1, neighborsB.zw);
	selectedUV = PickIfShallowestUV(selectedUV, shallowestDepth, depthC0, neighborsC.xy);

	drCenter = ClampScreenUV(texCoord, drMax);
	float depthCenter = depthTex.Sample(depthSampler, drCenter).x;
	shallowestDepth = min(depthCenter, shallowestDepth);
	float depthC1 = depthTex.Sample(depthSampler, drNeighborsC.zw).x;
	shallowestDepth = min(depthC1, shallowestDepth);

	selectedUV = PickIfShallowestUV(selectedUV, shallowestDepth, depthC1, neighborsC.zw);
	selectedUV = PickIfShallowestUV(selectedUV, shallowestDepth, depthCenter, texCoord);

	return selectedUV;
}

// Reproject the history-sample UV from velocity and report whether the reprojection left the frame
// (disocclusion → reject). VR reprojects within the current eye (mono-space velocity, per-eye clamp;
// see Stereo::ApplyVelocityToUV) so a pixel near the x=0.5 seam never samples the other eye's
// history; SE adds velocity in screen space and tests the raw UV against [0,1] (the same boolean the
// decompile computed inline). Folding the SE test in here lets main() treat both paths uniformly.
float2 ReprojectHistoryUV(float2 texCoord, float2 velocity, out bool outOfBounds)
{
#	ifdef VR
	float2 prevUV = Stereo::ApplyVelocityToUV(texCoord, velocity, outOfBounds);
	return FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(prevUV);
#	else
	float2 prevUV = texCoord + velocity;
	outOfBounds = any(prevUV >= 1.0) || any(prevUV <= 0.0);
	return ClampHistoryUV(prevUV);
#	endif
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	float2 texCoord = input.TexCoord;
	float4 colorOut, feedbackOut;

	// float4 packs — component reuse matches vanilla decompile (see header comment).
	float4 motionReject, corner;  // decompile r0–r4 (corner = depth-guided tap; motionReject = velocity/UV)

	float2 drMax = GetDynamicResolutionMax(), drUVMin, drUVMax, drCenter;
	float4 drNeighborsA, drNeighborsB, drNeighborsC;

	motionReject.xy = SelectDepthGuidedUV(
		texCoord,
		drMax,
		drUVMin,
		drUVMax,
		drCenter,
		drNeighborsA,
		drNeighborsB,
		drNeighborsC,
		corner.xyz);

	// --- motion vector and history sample ---
	motionReject.xy = velocityTex.Sample(velocitySampler, ClampScreenUV(motionReject.xy, drMax)).xy;
	// Reproject history; prevUVOutOfBounds feeds the disocclusion reject below (both VR and SE).
	bool prevUVOutOfBounds;
	float2 historyUV = ReprojectHistoryUV(texCoord.xy, motionReject.xy, prevUVOutOfBounds);
	float motionLength = sqrt(dot(motionReject.xy, motionReject.xy));
	// Feedback RT round-trip: .x = prev feedback luma, .y = prev historyFeedback weight, .z = prev
	// motionConfidence (see feedbackOut writes at the end). The two weights are NOT colour/luma.
	float3 historySample = historyTex.Sample(historySampler, historyUV).xyz;
	float historyLuma = historySample.x;
	float historyFeedbackPrev = historySample.y;
	float prevMotionConfidence = historySample.z;
#	ifdef HDR_OUTPUT
	// historyLuma is stored as game-gamma (see EncodeFeedbackLuma on write); decode to PQ to match
	// the working space of all neighbour taps. The two weights are not luma — do NOT convert them.
	historyLuma = DecodeFeedbackLuma(historyLuma);
#	endif
	corner.w = dot(corner.xzy, kLumaWeights);
	float cornerBelowHist = cmp(corner.w < historyLuma);  // toggles corner-tap inclusion in the AABB

	// --- neighbour colour / luma samples ---
	// Sample the 9-tap neighbourhood (packed GRB.xyz + luma.w) plus each tap's below-history mask,
	// in the vanilla sample order so behaviour is bit-exact. Index map (also used by the brackets
	// and flicker below): [0]=uvMin, [1]=A0, [2]=A1, [3]=B0, [4]=B1, [5]=C0, [6]=C1. The decompile
	// scattered the masks through reused .x slots; here they live in a parallel array.
	const float2 neighborUVs[7] = {
		drUVMin, drNeighborsA.xy, drNeighborsA.zw, drNeighborsB.xy, drNeighborsB.zw, drNeighborsC.xy, drNeighborsC.zw
	};
	float4 neighbors[7];
	float neighborBelowHist[7];
	{
		ISTAA_NeighborTap tap = SampleNeighborGRB(drUVMin, historyLuma);
		neighbors[0] = PackNeighborTap(tap);
		neighborBelowHist[0] = tap.belowHist;
	}
	float uvMinCoverage = AlphaCoverageMask(drUVMin);  // seeds the allTransparent test far below
	[unroll] for (int n = 1; n < 7; n++)
	{
		ISTAA_NeighborTap tap = SampleNeighborGRB(neighborUVs[n], historyLuma);
		neighbors[n] = PackNeighborTap(tap);
		neighborBelowHist[n] = tap.belowHist;
	}
	float3 centerColor = SampleCenterRGB(drCenter);  // centre RGB

	// --- centre bracket seed, neighbourhood bracket, flicker, temporal blend ---
	float centerLuma = dot(centerColor.yzx, kLumaWeights);
	float belowHistCentre = cmp(centerLuma < historyLuma);
	// Centre colour packed as (R, B, luma) — the bracket seeds clamp this against the luma caps.
	float3 centerRBL = float3(centerColor.x, centerColor.z, centerLuma);
	// Bracket ceiling: 1.001 is just above the maximum PQ value (1.0 = 10000 nits).
	// Nothing in the scene exceeds this, so the ceiling works correctly in PQ working space.
	// (In linear BT2020 this would be wrong — sky/specular exceed 1.0 linear — but PQ is bounded.)
	// Seed the min bracket: centre colour (as R,B,luma) clamped to the 1.001 ceiling; if the centre is
	// below history (belowHistCentre), start at the ceiling. Then fold C1 (neighbors[6]).
	float3 minBracket = cmp(centerLuma < kMaxLumaCap) ? centerRBL : kMaxLumaCap.xxx;
	minBracket = belowHistCentre ? kMaxLumaCap.xxx : minBracket;
	minBracket = MergeLumaBracket(neighbors[6], minBracket, neighborBelowHist[6]);

	// --- neighborhood min/max color bracket ---
	// 4-tap weighted neighbour colour (C + D + L + LD); consumed by the non-reject output path.
	float3 neighborColor = NeighborWeights.zzz * neighbors[5].yxz;           // C0
	neighborColor = neighbors[4].yxz * NeighborWeights.www + neighborColor;  // B1
	neighborColor = neighbors[6].yxz * NeighborWeights.yyy + neighborColor;  // C1
	neighborColor = centerColor * NeighborWeights.xxx + neighborColor;
	// Both the min and max colour brackets fold the same six taps in vanilla order — C0,B1,B0,A1,A0,
	// uvMin = neighbors[5..0] — gated by each tap's belowHist; only the merge rule and seed differ.
	// Running min-luma bracket: each tap committed unless its gate is set (see MergeLumaBracket).
	[unroll] for (int fold = 5; fold >= 0; fold--)
		minBracket = MergeLumaBracket(neighbors[fold], minBracket, neighborBelowHist[fold]);
	float3 minBoundNoCorner = minBracket;
	// Final fold: corner tap, ungated (gate 0 → always take the lower-luma colour).
	float3 minBoundWithCorner = MergeLumaBracket(corner, minBoundNoCorner, 0);
	// Low-clamp the (max-bracketed) C1 tap to the kMinLumaCap floor: build the centre-vs-floor bound
	// (gated by belowHistCentre), then fold C1 toward it (MergeMaxBracket, gated by C1's belowHist).
	float3 lowClamp = cmp(kMinLumaCap < centerLuma) ? centerRBL : kMinLumaCap.xxx;
	lowClamp = belowHistCentre ? lowClamp : kMinLumaCap.xxx;
	neighbors[6].xyz = MergeMaxBracket(neighbors[6], lowClamp, neighborBelowHist[6]);

	// --- flicker score: 4 minus one integer contribution per neighbourhood tap ---
	// Each contribution reads a tap's ORIGINAL .w luma; the colour sort below only mutates .w/.yzw
	// after this, so computing them here is behaviour-preserving. The sum is order-independent (each
	// contribution is a ceil() integer); tap order matches the original 8-term sum.
	const float4 flickerTaps[8] = { corner, neighbors[0], neighbors[1], neighbors[2], neighbors[3], neighbors[4], neighbors[5], neighbors[6] };
	float flickerScore = 4;
	[unroll] for (int flick = 0; flick < 8; flick++)
		flickerScore -= FlickerLumaContribution(centerLuma, flickerTaps[flick].w);
	flickerScore = saturate(flickerScore);

	// --- neighbourhood MAX-luma colour bracket (complement to the min bracket above) ---
	// Same six taps/order as the min fold (neighbors[5..0]), but the max rule commits a tap when its
	// belowHist gate is SET — inverted vs the min fold (see MergeMaxBracket). Seeded from the
	// low-clamped C1 colour.
	float3 maxBracket = neighbors[6].xyz;
	[unroll] for (int maxFold = 5; maxFold >= 0; maxFold--)
		maxBracket = MergeMaxBracket(neighbors[maxFold], maxBracket, neighborBelowHist[maxFold]);
	// Complete the max bracket (ungated corner fold), then select the neighbourhood AABB bounds for
	// history clamping. cornerBelowHist toggles whether the corner tap is included: when set, max uses
	// the with-corner bracket and min the without-corner one (opposite when clear).
	float3 maxWithCorner = MergeMaxBracket(corner, maxBracket, 1);
	float3 maxBoundSel = cornerBelowHist.xxx ? maxWithCorner : maxBracket;             // (R, B, luma)
	float3 minBoundSel = cornerBelowHist.xxx ? minBoundNoCorner : minBoundWithCorner;  // (R, B, luma)

	// --- temporal blend, clamp, and sharpen (history rectification toward the neighbourhood AABB) ---
	// Build two clip candidates (min/max bound, RCAS-sharpened), clamp history luma into their range,
	// compute the clip ratio, and lerp the colour toward the rectified value by it. historyBlend sets
	// how much history to keep; when low, clampToNeighborhood pulls the result toward the bracket.
	// Two history-clip candidates from the min/max AABB bounds, each packed (R, sharpenDelta, B, luma)
	// — the decompile stashes the RCAS sharpen in the G slot.
	float minOverbright = cmp(1 < minBoundSel.z);
	float4 clipLo = float4(minBoundSel.x, SharpenDelta(minBoundSel.z, minBoundSel.x, minBoundSel.y), minBoundSel.y, minBoundSel.z);  // min bound
	float4 clipHi = float4(maxBoundSel.x, SharpenDelta(maxBoundSel.z, maxBoundSel.x, maxBoundSel.y), maxBoundSel.y, maxBoundSel.z);  // max bound
	// Prefer the max bound; fall back to the min bound when the max luma is degenerate (<0), then to
	// that result vs the min bound when the min luma is overbright (>1).
	float4 clipPick = cmp(clipHi.w < 0) ? clipLo : clipHi;
	float4 clipFinal = minOverbright ? clipPick : clipLo;
	// Clamp history luma into the candidates' luma range: (clamped, lo, hi).
	float3 clampedHistory = float3(clamp(historyLuma, clipPick.w, clipFinal.w), clipPick.w, clipFinal.w);
	float3 historyLumaPack = float3(historyLuma, clipHi.w, clipLo.w);  // (history luma, max luma, min luma)
	float historyMotionDecay = kHistoryLumaDecay * historyFeedbackPrev;
	float historyBlend = saturate(flickerScore * 0.25 + historyMotionDecay);
	float clampToNeighborhood = cmp(historyBlend < kHistoryBlendThreshold);
	// clipState = (selected luma, lo, hi) — the candidate whose colour we'll rectify toward.
	float3 clipState = clampToNeighborhood.xxx ? clampedHistory : historyLumaPack;
	// Clip ratio: where the selected luma sits within its [lo, hi] range; 0.5 fallback when negligible.
	float clipRange = clipState.z - clipState.y;
	float clipRatio = cmp(kLumaEpsilon < clipRange) ? (clipState.x - clipState.y) / clipRange : 0.5;
	float3 rectifyLo = clampToNeighborhood.xxx ? clipPick.xyz : clipHi.xyz;
	float3 rectifyHi = clampToNeighborhood.xxx ? clipFinal.xyz : clipLo.xyz;
	// Rectified colour: lerp from the low candidate toward the high one by the clip ratio.
	float3 rectifiedColor = lerp(rectifyLo, rectifyHi, clipRatio.xxx);

	// --- disocclusion / mask rejection ---
	// Reproject OOB (computed per-eye in VR / screen-space in SE inside ReprojectHistoryUV), then
	// OR in the mask reject below.
	float reject = prevUVOutOfBounds ? 1 : 0;                      // disocclusion / OOB / mask rejection flag
	float2 maskValues = maskTex.Sample(maskSampler, drCenter).xy;  // .x depth-mask, .y coverage
	float centerCoverage = AlphaCoverageMask(drCenter);
	float maskReject = cmp(ThresholdParams.w < maskValues.y);
	reject = (int)reject | (int)maskReject;
	// Two colour candidates: the rectified history colour and the neighbourhood-weighted colour
	// (both collapse to the centre tap when the pixel is rejected).
	float3 workColor = reject.xxx ? centerColor : rectifiedColor;  // history/rectified colour, evolves below
	// On reject the history luma/motion collapse to the current frame (centre luma, zero motion).
	// Kept as one float2 select (.x luma, .y motion) to match the decompile's single movc.
	float2 blendLumaMotion = reject.xx ? float2(centerLuma, 0) : float2(clipState.x, prevMotionConfidence);
	float3 neighborBlend = reject.xxx ? centerColor : neighborColor;
	float3 centerVsNeighbor = centerColor + -neighborBlend;
	float motionNormScale = 128 * TexelSizeParams.x;  // 128-texel span used to normalize motion length
	float motionConfidence = saturate(motionLength / motionNormScale);
	float motionVsHistory = motionConfidence + -blendLumaMotion.y;
	// Luma convergence decay: the *20 and *100 constants were tuned for gamma-space luma.
	// PQ is perceptually uniform — a single linear rescale of the PQ diff is accurate
	// across all luminance levels (unlike converting through gamma, which has a varying
	// derivative and overcorrects at bright and dark extremes).
	// Scale factor: 0.05 gamma ≈ 0.020 PQ at mid-scene luminance → factor ≈ 2.5.
	// luma diff drives the history similarity weights (and the final luma fixup at blend end).
	float lumaDiff = blendLumaMotion.x + -centerLuma;
	// similarity = max(0, 1 - scale*diff) per channel: .x weights colour, .y the feedback luma.
	// Only the diff term differs by permutation: HDR keys off the luma diff (scaled to PQ), SDR off
	// the motion-vs-history delta.
#	ifdef HDR_OUTPUT
	float similarityDiff = abs(lumaDiff) * 0.05;
#	else
	float similarityDiff = abs(motionVsHistory);
#	endif
	float2 similarity = max(float2(0, 0), -similarityDiff.xx * kSimilarityScale + float2(1, 1));
	float3 targetColor = similarity.xxx * centerVsNeighbor + neighborBlend;
	workColor = -targetColor + workColor;
	float blendWeight = BlendParams.x + -BlendParams.y;
	blendWeight = motionConfidence * blendWeight + BlendParams.y;
	blendWeight = min(blendWeight, similarity.x);
	float historyFeedback = similarity.y * historyBlend;
	float feedbackComplement = kFeedbackBlendMax + -blendWeight;
	blendWeight = historyFeedback * feedbackComplement + blendWeight;
	feedbackOut.yz = float2(historyFeedback, motionConfidence);
	// outPacked.yzw = resolved colour; .x = feedback luma (set just below).
	float4 outPacked;
#	ifdef HDR_OUTPUT
	targetColor = max(targetColor, 0);
	workColor = saturate(blendWeight.xxx * workColor + targetColor);
	// Skip vanilla BlendParams.z/w detail recovery — neighbourhood delta blows up in linear HDR
	// and causes dark bezels / halos on the alpha-aware outPacked.yzw output path.
	outPacked.yzw = workColor;
#	else
	workColor = saturate(blendWeight.xxx * workColor + targetColor);

	float3 detailDelta = workColor + -neighborBlend;
	workColor = saturate(detailDelta * BlendParams.zzz + workColor);

	outPacked.xyz = neighborBlend + -workColor;
	outPacked.yzw = saturate(BlendParams.www * outPacked.xyz + workColor);
#	endif

	// Feedback luma: nudge centre luma by the blend-weighted luma diff, unless that nudge is negligible.
	float feedbackLuma = blendWeight * lumaDiff + centerLuma;
	float lumaNudge = blendWeight * lumaDiff;
	float lumaNudgeTiny = cmp(abs(lumaNudge) < kLumaEpsilon);
	outPacked.x = lumaNudgeTiny ? centerLuma : feedbackLuma;
	float outputLuma = dot(targetColor.yzx, kLumaWeights);

	// --- alpha-aware output ---
	// allTransparent: every 3x3 tap covered by alpha. Seed = uvMin coverage (uvMinCoverage),
	// gated by the 8 remaining taps (centerCoverage already holds the centre tap's coverage).
	const float2 alphaUVs[7] = {
		drNeighborsA.xy, drNeighborsA.zw, drNeighborsB.xy, drNeighborsB.zw,
		drNeighborsC.xy, drNeighborsC.zw, drUVMax
	};
	float allTransparent = uvMinCoverage;
	[unroll] for (int alphaTap = 0; alphaTap < 7; alphaTap++)
		allTransparent = AlphaCoverageMask(alphaUVs[alphaTap]) ? allTransparent : 0;
	allTransparent = centerCoverage ? allTransparent : 0;
	float depthMaskPass = cmp(ThresholdParams.w >= maskValues.x);
	float feedbackWeight = 1 + -maskValues.y;
	allTransparent = depthMaskPass ? allTransparent : 0;
	// .x = feedback luma, .yzw = resolved colour
	float4 resolved = allTransparent.xxxx ? float4(outputLuma, targetColor) : outPacked.xyzw;
	colorOut.xyz = resolved.yzw;
	float feedbackLumaOut = saturate(resolved.x * feedbackWeight);
#	ifdef HDR_OUTPUT
	// Encode PQ luma to game-gamma for feedback RT storage: raw PQ in a low-precision RT bands in
	// highlights (PQ packs high-nit values where quantization shows); game-gamma spreads precision
	// evenly — symmetric with DecodeFeedbackLuma on read.
	feedbackLumaOut = EncodeFeedbackLuma(feedbackLumaOut);
#	endif
	feedbackOut.x = feedbackLumaOut;
	// Vanilla writes opaque alpha unconditionally on both SE and VR (decompile o0.w = 1).
	colorOut.w = 1;
	feedbackOut.w = 1;

#	ifdef HDR_OUTPUT
	// colorOut is the display path — convert from PQ/BT2020 working space to game-gamma/BT709.
	// feedbackOut.x was already encoded above; do not modify it here.
	colorOut.xyz = ConvertRenderOutput(colorOut.xyz);
#	endif

	psout.Color = colorOut;
	psout.Feedback = feedbackOut;
	return psout;
}
#endif
