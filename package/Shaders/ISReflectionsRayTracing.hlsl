#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "Common/VRStereoEffects.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
SamplerState NormalSampler : register(s0);
SamplerState ColorSampler : register(s1);
SamplerState DepthSampler : register(s2);
SamplerState AlphaSampler : register(s3);

Texture2D<float4> NormalTex : register(t0);
Texture2D<float4> ColorTex : register(t1);
Texture2D<float4> DepthTex : register(t2);
Texture2D<float4> AlphaTex : register(t3);

// Shared Hi-Z chains bound by DynamicCubemaps::BuildHiZ. All four are unbound while the setting
// is off, and eye 1's pair on flat; SSRHiZAvailable keeps unbound chains out of the walk.
Texture2D<float> HiZMinEye0 : register(t115);
Texture2D<float> HiZMaxEye0 : register(t116);
Texture2D<float> HiZMinEye1 : register(t117);
Texture2D<float> HiZMaxEye1 : register(t118);

// Per-frame walk settings, bound with the chains so both always describe the same build.
cbuffer SSRHiZ : register(b9)
{
	uint SSRHiZAvailable;  // 1 once both chains were built and bound for this frame; 0 keeps the linear march
	uint SSRHiZMaxLevel;   // coarsest built mip level
	uint SSRHiZSizeX;      // valid base-level extent of one chain, in texels
	uint SSRHiZSizeY;
};

cbuffer PerGeometry : register(b2)
{
	float4 SSRParams : packoffset(c0);  // fReflectionRayThickness in x, fReflectionMarchingRadius in y, fAlphaWeight in z, 1 / fReflectionMarchingRadius in w
	float3 DefaultNormal : packoffset(c1);
};

static const int iterations = 64.0;
static const int binaryIterations = ceil(log2(iterations));

static const float rayLength = 1.0;

#	if defined(VR)
#		include "Common/FoveatedShaderDetail.hlsli"

static const int minFoveatedIterations = 16;

// Per-pixel SSR foveation weight from the active foveation mask (VRFoveationData0
// + per-eye center offset). 1 in the center, falling to 0 in the periphery.
float GetVRSSRFoveationWeight(float ssrFoveationMode, float2 eyeUv, uint eyeIndex)
{
	float2 centerOffset = eyeIndex == 0 ? SharedData::VRFoveationCenterOffsets.xy : SharedData::VRFoveationCenterOffsets.zw;
	return FoveatedEvaluateShaderDetailWeight(
		ssrFoveationMode,
		eyeUv,
		SharedData::VRFoveationData0.x,
		SharedData::VRFoveationData0.y,
		SharedData::VRFoveationData0.z,
		centerOffset);
}

// Scale the raymarch count by the foveation weight: full in the center, down to
// minFoveatedIterations toward the periphery.
int GetSSRRaymarchIterations(float foveationWeight)
{
	int iterationCount = (int)ceil(lerp((float)minFoveatedIterations, (float)iterations, saturate(foveationWeight)));
	return min(max(iterationCount, minFoveatedIterations), iterations);
}

int GetSSRBinaryIterations(int raymarchIterations)
{
	int iterationCount = (int)ceil(log2((float)raymarchIterations));
	return min(max(iterationCount, 1), binaryIterations);
}

// Finest level the Hi-Z walk may stop at: coarser in the periphery to match its shorter step
// budget, the base level in the center.
static const int peripheralHiZLevel = 2;

int GetSSRHiZFinestLevel(float foveationWeight)
{
	int level = (int)round((1.0 - saturate(foveationWeight)) * (float)peripheralHiZLevel);
	return min(level, (int)SSRHiZMaxLevel);
}
#	endif

/** Maps an eye-local ray sample to current-frame dynamic-resolution SBS coordinates. */
float2 ConvertRaySample(float2 raySample, uint eyeIndex, uint2 textureDimensions)
{
	float2 stereoUV = Stereo::ConvertToStereoUV(raySample, eyeIndex);
	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(stereoUV);
#	if defined(VR)
	screenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, textureDimensions, FrameBuffer::DynamicResolutionParams1.xy);
#	endif
	return screenPosition;
}

/** Maps an eye-local ray sample to previous-frame dynamic-resolution SBS coordinates. */
float2 ConvertRaySamplePrevious(float2 raySample, uint eyeIndex)
{
	float2 stereoUV = Stereo::ConvertToStereoUV(raySample, eyeIndex);
	float2 screenPosition = FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(stereoUV);
#	if defined(VR)
	screenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, AlphaTex, FrameBuffer::DynamicResolutionParams1.zw);
#	endif
	return screenPosition;
}

/** Tile of one chain at `level` that holds `uv`, and the ray parameter at which a ray through `uv`
 *  along the mono-UV slope `dir` leaves it. A still axis is clamped so it cannot divide by zero. */
void HiZTile(float2 uv, float2 dir, float2 size, int level, out int3 coord, out float tExit)
{
	float scale = exp2((float)level);
	// Mip-L texel i reduces mip-0 texels [i * 2^L, (i + 1) * 2^L), so the texel holding uv is the
	// exact ratio; the valid count only bounds the clamp and rounds up.
	int2 validSize = max(int2(ceil(size / scale)), int2(1, 1));
	int2 cell = clamp(int2(floor(uv * size / scale)), int2(0, 0), validSize - 1);
	coord = int3(cell, level);

	float2 uvSpan = scale / size;
	float2 dirSign = step(float2(0.0, 0.0), dir);
	float2 exitUV = lerp(float2(cell) * uvSpan, float2(cell + 1) * uvSpan, dirSign);
	float2 safeDir = max(abs(dir), float2(1e-6, 1e-6)) * (dirSign * 2.0 - 1.0);
	tExit = min((exitUV.x - uv.x) / safeDir.x, (exitUV.y - uv.y) / safeDir.y);
}

float HiZMinAt(uint eye, int3 coord) { return eye == 0 ? HiZMinEye0.Load(coord) : HiZMinEye1.Load(coord); }
float HiZMaxAt(uint eye, int3 coord) { return eye == 0 ? HiZMaxEye0.Load(coord) : HiZMaxEye1.Load(coord); }

/**
 * @brief Coarse walk over the shared Hi-Z min/max chains, from the ray origin to its first crossing.
 *
 * Ascends one level per tile proved empty and descends when a tile might hold the crossing. Uses
 * the same ray-versus-depth test as the linear march; the tiles only decide where it is asked.
 *
 * @return false when the ray left the frame or ended without a crossing.
 */
bool HiZCoarseMarch(
	float3 projReflectionDirection,
	float3 projPosition,
	uint eyeIndex,
	uint2 depthTextureDimensions,
	int stepBudget,
	int finestLevel,
	out float3 outPrevRaySample,
	out float3 outRaySample,
	out uint outHitEyeIndex)
{
	outPrevRaySample = projPosition;
	outRaySample = projPosition;
	outHitEyeIndex = eyeIndex;

	const float2 chainSize = float2(SSRHiZSizeX, SSRHiZSizeY);
	const int coarsestLevel = (int)SSRHiZMaxLevel;
	// Every advance has a floor of one linear step, so the walk always moves and always finishes
	// inside stepBudget, even when a sample lands exactly on a tile boundary.
	const float tStep = 1.0 / float(stepBudget);

	float t = 0.0;
	float tPrev = 0.0;
	int level = finestLevel;

	// Which eye's chain the tiles are read from. It follows the viewport-exit switch for free and
	// moves to the other eye once the max chain has shown the ray passed through its own occluder.
#	if defined(VR)
	bool useOtherEyeChain = false;
#	endif

	// The first crossing the max chain called a pass-through, kept so a walk that finds nothing
	// better still returns the hit the linear march would have returned.
	bool haveFallback = false;
	float fallbackT = 0.0;
	float fallbackPrevT = 0.0;
	uint fallbackEyeIndex = eyeIndex;

	[loop] for (int step = 0; step < stepBudget; ++step)
	{
		if (t >= 1.0)
			break;

		float3 raySample = projPosition + t * projReflectionDirection;

		float2 sampleUV;
		uint sampleEyeIndex;
		Stereo::ResolveMonoUVForEye(raySample, eyeIndex, sampleUV, sampleEyeIndex);

		// Breaking keeps the fallback crossing; returning false here drops hits the linear march finds.
		if (FrameBuffer::IsOutsideFrame(sampleUV))
			break;

		uint chainEye = sampleEyeIndex;
		float2 chainUV = sampleUV;
#	if defined(VR)
		if (useOtherEyeChain && sampleEyeIndex == eyeIndex) {
			float3 otherEyeSample = Stereo::ConvertMonoUVToOtherEye(raySample, eyeIndex);
			if (!FrameBuffer::IsOutsideFrame(otherEyeSample.xy)) {
				chainEye = 1 - eyeIndex;
				chainUV = otherEyeSample.xy;
			}
		}
#	endif

		// The tile's exit parameter is a slope, so it must be the ray's slope in the eye that owns
		// chainUV: the two eyes' slopes differ by parallax (the paper's rayDirEye2XY).
		float2 tileDirXY = projReflectionDirection.xy;
#	if defined(VR)
		if (chainEye != eyeIndex) {
			float3 aheadSample = Stereo::ConvertMonoUVToOtherEye(raySample + tStep * projReflectionDirection, eyeIndex);
			float2 otherEyeDir = (aheadSample.xy - chainUV) / tStep;
			// A degenerate or folded-back reprojection keeps this eye's slope, which still bounds the
			// tile where a NaN exit would not.
			if (all(isfinite(otherEyeDir)) && dot(otherEyeDir, otherEyeDir) > 1e-12)
				tileDirXY = otherEyeDir;
		}
#	endif

		int3 coord;
		float tCellExit;
		HiZTile(chainUV, tileDirXY, chainSize, level, coord, tCellExit);
		// A tile cannot extend the march past the end of the ray. Clamping here also keeps a still
		// screen axis, whose exit parameter is enormous, from scaling a zero depth slope into a NaN.
		tCellExit = clamp(tCellExit, 0.0, 1.0 - t);

		float tileMin = HiZMinAt(chainEye, coord);
		float tileMax = HiZMaxAt(chainEye, coord);

		// The ray's depth span over that tile.
		float zExit = raySample.z + tCellExit * projReflectionDirection.z;
		float rayMinZ = min(raySample.z, zExit);
		float rayMaxZ = max(raySample.z, zExit);

		float iterationDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(sampleUV, sampleEyeIndex, depthTextureDimensions), 0).x;

		if (saturate((raySample.z - iterationDepth) / SSRParams.y) > 0.0) {
			// Behind every surface in this tile as well: the ray is inside or past the occluder
			// rather than landing on it, so this is not the crossing.
			if (rayMinZ > tileMax) {
				if (!haveFallback) {
					haveFallback = true;
					fallbackT = t;
					fallbackPrevT = tPrev;
					fallbackEyeIndex = sampleEyeIndex;
				}
#	if defined(VR)
				useOtherEyeChain = true;
#	endif
				tPrev = t;
				t += (level > finestLevel) ? max(tCellExit, tStep) : tStep;
				level = min(level + 1, coarsestLevel);
				continue;
			}

			// Descend first: a crossing is only handed on once the tile holding it is one base-level
			// block, so the interval the binary search refines starts from the last advance.
			if (level > finestLevel) {
				level -= 1;
				continue;
			}

			outPrevRaySample = projPosition + tPrev * projReflectionDirection;
			outRaySample = raySample;
			outHitEyeIndex = sampleEyeIndex;
			return true;
		}

		if (rayMaxZ < tileMin) {
			// The span is wholly in front of every surface here: nothing to cross inside, so step
			// over the tile and look wider next time.
			tPrev = t;
			t += (level > finestLevel) ? max(tCellExit, tStep) : tStep;
			level = min(level + 1, coarsestLevel);
			continue;
		}

		if (level > finestLevel) {
			level -= 1;
			continue;
		}

		tPrev = t;
		t += tStep;
	}

	if (!haveFallback)
		return false;

	outPrevRaySample = projPosition + fallbackPrevT * projReflectionDirection;
	outRaySample = projPosition + fallbackT * projReflectionDirection;
	outHitEyeIndex = fallbackEyeIndex;
	return true;
}

float4 GetReflectionColor(
	float3 projReflectionDirection,
	float3 projPosition,
	uint eyeIndex,
	uint2 depthTextureDimensions
#	if defined(VR)
	,
	int raymarchIterations,
	int binaryIterationsCount,
	float foveationWeight
#	endif
)
{
	float4 result = 0.0;
	float3 prevRaySample = projPosition;
	float3 raySample = projPosition;
	uint hitEyeIndex = eyeIndex;
	float2 sampleUV;
	float iterationDepth = 0.0;
	bool found = false;

	// VR scales the raymarch/binary counts by the foveation weight and fades the result; non-VR uses
	// full counts. Bounds are runtime in VR, so [loop] is required (cannot unroll).
#	if defined(VR)
	int rayCount = raymarchIterations;
	int binCount = binaryIterationsCount;
	float fovWeight = foveationWeight;
#	else
	int rayCount = iterations;
	int binCount = binaryIterations;
	float fovWeight = 1.0;
#	endif

	// Hi-Z chains when built for this frame, the original fixed-step linear march otherwise.
	[branch] if (SSRHiZAvailable != 0)
	{
#	if defined(VR)
		found = HiZCoarseMarch(projReflectionDirection, projPosition, eyeIndex, depthTextureDimensions,
			rayCount, GetSSRHiZFinestLevel(foveationWeight), prevRaySample, raySample, hitEyeIndex);
#	else
		found = HiZCoarseMarch(projReflectionDirection, projPosition, eyeIndex, depthTextureDimensions,
			rayCount, 0, prevRaySample, raySample, hitEyeIndex);
#	endif
	}
	else
	{
#	if defined(VR)
		[loop] for (int i = 0; i < rayCount; i++)
		{
#	else
		for (int i = 0; i < rayCount; i++) {
#	endif
			prevRaySample = raySample;
			raySample = projPosition + (float(i) / float(rayCount)) * projReflectionDirection;

			uint sampleEyeIndex;
			Stereo::ResolveMonoUVForEye(raySample, eyeIndex, sampleUV, sampleEyeIndex);

			if (FrameBuffer::IsOutsideFrame(sampleUV))
				break;

			iterationDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(sampleUV, sampleEyeIndex, depthTextureDimensions), 0).x;

			if (saturate((raySample.z - iterationDepth) / SSRParams.y) > 0.0) {
				hitEyeIndex = sampleEyeIndex;
				found = true;
				break;
			}
		}
	}

	if (found) {
		float3 binaryMinRaySample = prevRaySample;
		float3 binaryMaxRaySample = raySample;
		float3 binaryRaySample = raySample;
		float depthThicknessFactor = 0.0;

#	if defined(VR)
		[loop] for (int k = 0; k < binCount; k++)
		{
#	else
		for (int k = 0; k < binCount; k++) {
#	endif
			binaryRaySample = lerp(binaryMinRaySample, binaryMaxRaySample, 0.5);

			Stereo::ResolveMonoUVForEye(binaryRaySample, eyeIndex, sampleUV, hitEyeIndex);
			iterationDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(sampleUV, hitEyeIndex, depthTextureDimensions), 0).x;

			// Compute expected depth vs actual depth
			depthThicknessFactor = 1.0 - saturate(abs(binaryRaySample.z - iterationDepth) / SSRParams.y);

			if (iterationDepth < binaryRaySample.z)
				binaryMaxRaySample = binaryRaySample;
			else
				binaryMinRaySample = binaryRaySample;
		}

		// Fade based on ray length
		float ssrMarchingRadiusFadeFactor = 1.0 - saturate(length(binaryRaySample - projPosition) / rayLength);

		float2 uvResultScreenCenterOffset = binaryRaySample.xy - 0.5;

#	ifdef VR
		float2 centerDistance = abs(uvResultScreenCenterOffset.xy * 2.0);

		// Make VR fades consistent by taking the closer of the two eyes
		// Based on concepts from https://cuteloong.github.io/publications/scssr24/
		float2 otherEyeUvResultScreenCenterOffset = Stereo::ConvertMonoUVToOtherEye(float3(binaryRaySample.xy, iterationDepth), eyeIndex).xy - 0.5;
		centerDistance = min(centerDistance, abs(otherEyeUvResultScreenCenterOffset * 2.0));
#	else
		float2 centerDistance = abs(uvResultScreenCenterOffset.xy * 2.0);
#	endif

		// Fade out around screen edges
		float centerDistanceFadeFactorX = smoothstep(0.0, 0.1, saturate(1.0 - centerDistance.x));
		float centerDistanceFadeFactorY = smoothstep(0.0, 0.5, saturate(1.0 - centerDistance.y));

		float fadeFactor = depthThicknessFactor * ssrMarchingRadiusFadeFactor * centerDistanceFadeFactorX * centerDistanceFadeFactorY;

		if (fadeFactor > 0.0) {
			// Resolve final UV in the eye that owns the hit
			float2 finalSampleUV;
			uint finalEyeIndex;
			Stereo::ResolveMonoUVForEye(float3(binaryRaySample.xy, iterationDepth), eyeIndex, finalSampleUV, finalEyeIndex);

			uint2 colorTextureDimensions = uint2(1, 1);
#	if defined(VR)
			ColorTex.GetDimensions(colorTextureDimensions.x, colorTextureDimensions.y);
#	endif
			float2 colorScreenPosition = ConvertRaySample(finalSampleUV, finalEyeIndex, colorTextureDimensions);
			float3 color = ColorTex.SampleLevel(ColorSampler, colorScreenPosition, 0).xyz;
			if (ENABLE_LL && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::GammaRenderTarget))
				color = Color::SceneGammaToLinear(color);

			// Final sample to world-space
			float4 positionWS = float4(float2(finalSampleUV.x, 1.0 - finalSampleUV.y) * 2.0 - 1.0, iterationDepth, 1.0);
			positionWS = mul(FrameBuffer::CameraViewProjInverse[finalEyeIndex], positionWS);
			positionWS.xyz = positionWS.xyz / positionWS.w;
			positionWS.w = 1.0;

			// Compute camera motion vector
			float2 cameraMotionVector = MotionBlur::GetSSMotionVector(positionWS, positionWS, finalEyeIndex);

			// Reproject alpha from previous frame
			float2 reprojectedRaySample = finalSampleUV + cameraMotionVector;
			float4 alpha = 0.0;

			// Check that the reprojected data is within the frame
			if (!FrameBuffer::IsOutsideFrame(reprojectedRaySample.xy))
				alpha = float4(AlphaTex.SampleLevel(AlphaSampler, ConvertRaySamplePrevious(reprojectedRaySample.xy, finalEyeIndex), 0).xyz, 1.0);

			float3 reflectionColor = color + SSRParams.z * alpha.xyz * alpha.w;
			result = float4(reflectionColor, fadeFactor * fovWeight);
		}
	}

	return result;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	psout.Color = 0;

#	ifndef ENABLESSR
	// Disable SSR raymarch
	return psout;
#	endif

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(input.TexCoord);
	float2 uv = input.TexCoord;
	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
	float2 normalScreenPosition = screenPosition;
	float2 depthScreenPosition = screenPosition;

	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

#	if defined(VR)
	uint2 depthTextureDimensions;
	DepthTex.GetDimensions(depthTextureDimensions.x, depthTextureDimensions.y);
	normalScreenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, NormalTex, FrameBuffer::DynamicResolutionParams1.xy);
	depthScreenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, depthTextureDimensions, FrameBuffer::DynamicResolutionParams1.xy);

	float ssrFoveationWeight = 1.0;
	float ssrFoveationMode = SharedData::VRFoveationData0.w;
	[branch] if (ssrFoveationMode >= FOVEATED_SHADER_DETAIL_MODE_FEATHERED)
	{
		ssrFoveationWeight = GetVRSSRFoveationWeight(ssrFoveationMode, uv, eyeIndex);
		// Outside the foveation mask: skip SSR entirely. The cubemap/water
		// reflection fallback already covers these pixels.
		[branch] if (!FoveatedIsShaderDetailActive(ssrFoveationWeight)) return psout;
	}
#	endif

	[branch] if (NormalTex.Sample(NormalSampler, normalScreenPosition).z <= 0)
	{
		return psout;
	}

	float3 viewNormal = DefaultNormal;

	float depth = DepthTex.SampleLevel(DepthSampler, depthScreenPosition, 0).x;

	float4 positionVS = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
	positionVS = mul(FrameBuffer::CameraProjInverse[eyeIndex], positionVS);
	positionVS.xyz = positionVS.xyz / positionVS.w;

	float3 viewPosition = positionVS.xyz;
	float3 viewDirection = normalize(viewPosition);

	float3 reflectionDirection = reflect(viewDirection, viewNormal);
	float viewAttenuation = saturate(dot(viewDirection, reflectionDirection));
	[branch] if (viewAttenuation < 0)
	{
		return psout;
	}

	float4 reflectionPosition = float4(viewPosition + reflectionDirection, 1.0);
	float4 projReflectionPosition = mul(FrameBuffer::CameraProj[eyeIndex], reflectionPosition);
	projReflectionPosition /= projReflectionPosition.w;
	projReflectionPosition.xy = projReflectionPosition.xy * float2(0.5, -0.5) + float2(0.5, 0.5);

	float3 projPosition = float3(uv, depth);
	float3 projReflectionDirection = normalize(projReflectionPosition.xyz - projPosition) * rayLength;

#	if defined(VR)
	int raymarchIterations = iterations;
	int binaryIterationsCount = binaryIterations;
	[branch] if (ssrFoveationWeight < 0.9999)
	{
		raymarchIterations = GetSSRRaymarchIterations(ssrFoveationWeight);
		binaryIterationsCount = GetSSRBinaryIterations(raymarchIterations);
	}
	psout.Color = GetReflectionColor(
		projReflectionDirection, projPosition, eyeIndex, depthTextureDimensions,
		raymarchIterations, binaryIterationsCount, ssrFoveationWeight);
#	else
	psout.Color = GetReflectionColor(projReflectionDirection, projPosition, eyeIndex, uint2(1, 1));
#	endif

	return psout;
}
#endif
