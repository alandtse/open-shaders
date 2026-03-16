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
	float4 Tuning0;  // x=centerScale, y=centerFeather, z=useMipBias, w=mipBiasStrength
	float4 Tuning1;  // x=useEdgeBlur, y=edgeBlurStrength, z=edgeSensitivity, w=useOuterRingBlur
	float4 Tuning2;  // x=useJitterAttenuation, y=jitterAttenuationStrength, z=usePeripheryTAA, w=pad
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
	const float useMipBias = Tuning0.z;
	const float mipBiasStrength = Tuning0.w;
	const float useEdgeBlur = Tuning1.x;
	const float edgeBlurStrength = Tuning1.y;
	const float edgeSensitivity = Tuning1.z;
	const float useOuterRingBlur = Tuning1.w;
	const float useJitterAttenuation = Tuning2.x;
	const float jitterAttenuationStrength = Tuning2.y;
	const float usePeripheryTAA = Tuning2.z;

	float centerWeight = FoveatedComputeCenterBlendWeight(uv, centerScale, centerFeather);
	float peripheryWeight = saturate(1.0 - centerWeight);

	float2 jitterApplied = usePeripheryTAA > 0.5 ? Jitter : float2(0.0, 0.0);
	if (usePeripheryTAA > 0.5 && useJitterAttenuation > 0.5) {
		float attenuation = saturate(jitterAttenuationStrength) * peripheryWeight;
		jitterApplied *= (1.0 - attenuation);
	}

	float2 sourceUV = (uv * SourceScale + SourceOffset) - (jitterApplied * InvSourceDim);
	sourceUV = saturate(sourceUV);

	float mipBias = 0.0;
	if (useMipBias > 0.5) {
		float2 centeredUv = (uv - 0.5) * 2.0;
		float radialWeight = saturate(length(centeredUv));
		float biasWeight = max(peripheryWeight, radialWeight * peripheryWeight);
		mipBias = mipBiasStrength * biasWeight;
	}

	float4 centerSample = InputColor.SampleLevel(LinearSampler, sourceUV, mipBias);
	float4 outSample = centerSample;

	if (useEdgeBlur > 0.5 && edgeBlurStrength > 0.001 && peripheryWeight > 0.001) {
		float2 blurStep = InvOutputDim;
		float centerLuma = Luma(centerSample.rgb);
		float3 accum = centerSample.rgb;
		float accumWeight = 1.0;

		static const float2 kOffsets[4] = {
			float2(1.0, 0.0),
			float2(-1.0, 0.0),
			float2(0.0, 1.0),
			float2(0.0, -1.0)
		};

		[unroll]
		for (uint i = 0; i < 4; ++i) {
			float2 tapUV = saturate(sourceUV + kOffsets[i] * blurStep);
			float4 tap = InputColor.SampleLevel(LinearSampler, tapUV, mipBias);
			float tapLuma = Luma(tap.rgb);
			float edgeWeight = exp2(-abs(tapLuma - centerLuma) * edgeSensitivity);
			float weight = 0.85 * edgeWeight;
			accum += tap.rgb * weight;
			accumWeight += weight;
		}

		float3 blurred = accum / max(accumWeight, 1e-4);
		float blurBlend = saturate(edgeBlurStrength * peripheryWeight);
		outSample.rgb = lerp(centerSample.rgb, blurred, blurBlend);
	}

	// Optional second blur stage for the far outer ring only.
	if (useOuterRingBlur > 0.5 && useEdgeBlur > 0.5 && edgeBlurStrength > 0.001) {
		const float outerRingStart = 0.60;
		float outerRingWeight = saturate((peripheryWeight - outerRingStart) / (1.0 - outerRingStart));
		if (outerRingWeight > 0.001) {
			float2 blurStepOuter = InvOutputDim * 2.0;
			float centerLumaOuter = Luma(outSample.rgb);
			float3 accumOuter = outSample.rgb;
			float accumOuterWeight = 1.0;

			static const float2 kOuterOffsets[8] = {
				float2(1.0, 0.0),
				float2(-1.0, 0.0),
				float2(0.0, 1.0),
				float2(0.0, -1.0),
				float2(1.0, 1.0),
				float2(-1.0, 1.0),
				float2(1.0, -1.0),
				float2(-1.0, -1.0)
			};

			[unroll]
			for (uint i = 0; i < 8; ++i) {
				float2 tapUV = saturate(sourceUV + kOuterOffsets[i] * blurStepOuter);
				float4 tap = InputColor.SampleLevel(LinearSampler, tapUV, mipBias);
				float tapLuma = Luma(tap.rgb);
				float edgeWeight = exp2(-abs(tapLuma - centerLumaOuter) * (edgeSensitivity * 0.75));
				float weight = 0.7 * edgeWeight;
				accumOuter += tap.rgb * weight;
				accumOuterWeight += weight;
			}

			float3 outerBlurred = accumOuter / max(accumOuterWeight, 1e-4);
			float outerBlend = saturate(edgeBlurStrength * outerRingWeight);
			outSample.rgb = lerp(outSample.rgb, outerBlurred, outerBlend);
		}
	}

	OutColor[outputPos] = outSample;
}
