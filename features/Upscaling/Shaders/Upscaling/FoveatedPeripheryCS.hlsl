#include "Common/FoveatedMask.hlsli"

cbuffer FoveatedPeripheryCB : register(b0)
{
	float2 OutputDim;
	float2 InvOutputDim;
	float2 InvSourceDim;
	float2 SourceScale;
	float2 SourceOffset;
	float2 DispatchDim;
	float2 OutputOffset;
	float2 Jitter;
	float2 CenterOffset;
	float2 Pad0;
	float4 Tuning0;  // x=centerScale, y=centerFeather, z/w reserved
	float4 Tuning1;  // x=useEdgeBlur, y=edgeBlurStrength, z=edgeSensitivity, w reserved
	float4 Tuning2;  // x=visualizeMask, y/z/w reserved
};

Texture2D<float4> InputColor : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> OutColor : register(u0);

float Luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint2 localPos = dispatchID.xy;
	if (any(localPos >= uint2(DispatchDim)))
		return;

	uint2 outputPos = localPos + uint2(OutputOffset + 0.5);
	if (any(outputPos >= uint2(OutputDim)))
		return;

	float2 uv = (float2(outputPos) + 0.5) * InvOutputDim;

	const float centerScale = Tuning0.x;
	const float centerFeather = Tuning0.y;
	const float useEdgeBlur = Tuning1.x;
	const float edgeBlurStrength = Tuning1.y;
	const float edgeSensitivity = Tuning1.z;
	const float visualizeMask = Tuning2.x;

	float centerWeight = FoveatedComputeCenterBlendWeight(uv, centerScale, centerFeather, CenterOffset);
	float peripheryWeight = saturate(1.0 - centerWeight);

	if (visualizeMask > 0.5) {
		const float centerRadius = max(FoveatedClampCenterArea(centerScale) * 0.5, FOVEATED_CENTER_FEATHER_MIN);
		const float normalizedFeather = max(centerFeather, FOVEATED_CENTER_FEATHER_MIN) / centerRadius;
		const float ellipseDistance = FoveatedComputeEllipseDistance(uv, centerScale, CenterOffset);

		static const float3 kCenterColor = float3(0.20, 0.34, 0.28);
		static const float3 kFeatherColor = float3(0.43, 0.35, 0.20);
		static const float3 kPeripheryColor = float3(0.17, 0.22, 0.31);

		float3 maskColor = kPeripheryColor;
		if (ellipseDistance <= 1.0) {
			maskColor = kCenterColor;
		} else if (ellipseDistance <= (1.0 + normalizedFeather)) {
			maskColor = kFeatherColor;
		}

		OutColor[outputPos] = float4(maskColor, 1.0);
		return;
	}

	float2 sourceUV = (uv * SourceScale + SourceOffset) - (Jitter * InvSourceDim);
	sourceUV = saturate(sourceUV);

	float4 centerSample = InputColor.SampleLevel(LinearSampler, sourceUV, 0.0);
	float4 outSample = centerSample;

	if (useEdgeBlur > 0.5 && edgeBlurStrength > 0.001 && peripheryWeight > 0.001) {
		float2 blurStep = InvOutputDim;
		float centerLuma = Luma(centerSample.rgb);
		float3 accum = centerSample.rgb;
		float accumWeight = 1.0;
		const bool useReducedInnerKernel = peripheryWeight < 0.45;

		static const float2 kOffsets[4] = {
			float2(1.0, 0.0),
			float2(-1.0, 0.0),
			float2(0.0, 1.0),
			float2(0.0, -1.0)
		};

		if (useReducedInnerKernel) {
			// Alternate between horizontal and vertical 2-tap kernels to avoid directional bias.
			const bool useHorizontalAxis = ((outputPos.x ^ outputPos.y) & 1u) == 0u;
			float2 axis = useHorizontalAxis ? float2(1.0, 0.0) : float2(0.0, 1.0);
			float2 tapUV0 = saturate(sourceUV + axis * blurStep);
			float2 tapUV1 = saturate(sourceUV - axis * blurStep);
			float4 tap0 = InputColor.SampleLevel(LinearSampler, tapUV0, 0.0);
			float4 tap1 = InputColor.SampleLevel(LinearSampler, tapUV1, 0.0);
			float tapLuma0 = Luma(tap0.rgb);
			float tapLuma1 = Luma(tap1.rgb);
			float edgeWeight0 = exp2(-abs(tapLuma0 - centerLuma) * edgeSensitivity);
			float edgeWeight1 = exp2(-abs(tapLuma1 - centerLuma) * edgeSensitivity);
			float weight0 = 0.95 * edgeWeight0;
			float weight1 = 0.95 * edgeWeight1;
			accum += tap0.rgb * weight0 + tap1.rgb * weight1;
			accumWeight += weight0 + weight1;
		} else {
			[unroll]
			for (uint i = 0; i < 4; ++i) {
				float2 tapUV = saturate(sourceUV + kOffsets[i] * blurStep);
				float4 tap = InputColor.SampleLevel(LinearSampler, tapUV, 0.0);
				float tapLuma = Luma(tap.rgb);
				float edgeWeight = exp2(-abs(tapLuma - centerLuma) * edgeSensitivity);
				float weight = 0.85 * edgeWeight;
				accum += tap.rgb * weight;
				accumWeight += weight;
			}
		}

		float3 blurred = accum / max(accumWeight, 1e-4);
		float blurBlend = saturate(edgeBlurStrength * peripheryWeight);
		outSample.rgb = lerp(centerSample.rgb, blurred, blurBlend);
	}

	OutColor[outputPos] = outSample;
}
