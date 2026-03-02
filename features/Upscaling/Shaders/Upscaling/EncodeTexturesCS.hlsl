#include "Common/SharedData.hlsli"

cbuffer UpscalingData : register(b0)
{
	float2 DispatchDim;
	float2 TrueSamplingDim;  // BufferDim.xy * ResolutionScale
	float2 InvTrueSamplingDim;
	float SeamCenterX;
	float SeamHalfWidthPx;
	float MaskDepthThreshold;
	float VRSeamHardening;
	float2 SourceOffset;
	float2 pad0;
};

Texture2D<float2> TAAMask : register(t0);
Texture2D<float4> NormalsWaterMask : register(t1);
Texture2D<float2> MotionVectorMask : register(t2);
Texture2D<float> DepthMask : register(t3);

RWTexture2D<float> ReactiveMask : register(u0);
RWTexture2D<float> TransparencyCompositionMask : register(u1);
RWTexture2D<float2> MotionVectorOutput : register(u2);

float IsMaskedDepth(float depth)
{
	return depth <= MaskDepthThreshold ? 1.0 : 0.0;
}

float ComputeMaskEdgeFactor(uint2 sourcePos)
{
	static const int2 offsets[4] = {
		int2(1, 0),
		int2(-1, 0),
		int2(0, 1),
		int2(0, -1)
	};

	float centerMasked = IsMaskedDepth(DepthMask[sourcePos]);
	float edge = 0.0;

	[unroll]
	for (uint i = 0; i < 4; ++i) {
		int2 samplePos = int2(sourcePos) + offsets[i];
		if (any(samplePos < 0) || any(samplePos >= int2(TrueSamplingDim)))
			continue;

		float neighborMasked = IsMaskedDepth(DepthMask[samplePos]);
		edge = max(edge, abs(neighborMasked - centerMasked));
	}

	return edge;
}

float ComputeSeamFactor(uint2 sourcePos)
{
	float seamHalfWidth = max(SeamHalfWidthPx, 0.0001);
	float seamCenterUv = SeamCenterX * InvTrueSamplingDim.x;
	float pixelUv = (float(sourcePos.x) + 0.5) * InvTrueSamplingDim.x;
	float seamDistance = abs(pixelUv - seamCenterUv) * TrueSamplingDim.x;
	return saturate((seamHalfWidth - seamDistance) / seamHalfWidth);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint2 localPos = dispatchID.xy;
	if (any(localPos >= uint2(DispatchDim)))
		return;

	uint2 sourceOffset = uint2(SourceOffset + 0.5);
	uint2 sourcePos = localPos + sourceOffset;

	float2 taaMask = TAAMask[sourcePos];
	float transparencyCompositionMask = NormalsWaterMask[sourcePos].z;
	float reactiveMask = taaMask.x * 0.1 + taaMask.y;

#if defined(DLSS) || defined(FSR)
	float2 motionVector = MotionVectorMask[sourcePos];
	float2 outputMotionVector = motionVector;
#endif

#if defined(DLSS)
	float depth = DepthMask[sourcePos];
	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, SharedData::GetScreenDepth(depth));

	// Find longest motion vector in 5x5 neighborhood
	float2 longestMotionVector = motionVector;
	float maxMotionLengthSq = dot(motionVector, motionVector);

	[unroll]
	for (int y = -2; y <= 2; y++) {
		[unroll]
		for (int x = -2; x <= 2; x++) {
			int2 samplePos = int2(sourcePos) + int2(x, y);

			// Skip samples outside true sampling dimensions
			if (any(samplePos < 0) || any(samplePos >= int2(TrueSamplingDim)))
				continue;

			float neighborDepth = DepthMask[samplePos];

			// Take neighbor if it's longer AND closer
			if (neighborDepth < depth){
				float2 neighborMotionVector = MotionVectorMask[samplePos];

				// Square motion vector for length
				float motionLengthSq = dot(neighborMotionVector, neighborMotionVector);

				if (motionLengthSq > maxMotionLengthSq){
					maxMotionLengthSq = motionLengthSq;
					longestMotionVector = neighborMotionVector;
				}
			}
		}
	}

	outputMotionVector = lerp(longestMotionVector, motionVector, nearFactor);
#endif

	if (VRSeamHardening > 0.5) {
		float seamFactor = ComputeSeamFactor(sourcePos);
		float maskEdgeFactor = ComputeMaskEdgeFactor(sourcePos);

#if defined(DLSS) || defined(FSR)
		// Reduce temporal reprojection confidence near eye seam and HMD mask edges.
		float seamScale = lerp(1.0, 0.25, seamFactor);
		float maskScale = lerp(1.0, 0.0, maskEdgeFactor);
		outputMotionVector *= min(seamScale, maskScale);
#endif

		float hardeningMask = max(seamFactor * 0.6, maskEdgeFactor);
		reactiveMask = max(reactiveMask, hardeningMask);
		transparencyCompositionMask = max(transparencyCompositionMask, hardeningMask);
	}

#if defined(DLSS) || defined(FSR)
	MotionVectorOutput[localPos] = outputMotionVector;
#endif

	ReactiveMask[localPos] = reactiveMask;

	TransparencyCompositionMask[localPos] = transparencyCompositionMask;
}
