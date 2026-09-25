#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Skylighting/Skylighting.hlsli"

Texture2D<unorm float> srcOcclusionDepth : register(t0);
#if !defined(OCCLUSION_ONLY)
Texture2DArray<float4> ESRAMShadow : register(t3);

// Must stay byte-identical to the canonical mirror in ShadowSampling.hlsli --
// a StructuredBuffer bind requires matching element stride, not just a matching prefix.
struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
	column_major float4x4 FocusShadowProj[4];
	uint FocusShadowCount;
	uint3 _pad0;
};
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t2);
#endif

RWTexture3D<sh2> outProbeArray : register(u0);
RWTexture3D<uint> outAccumFramesArray : register(u1);
#if !defined(OCCLUSION_ONLY)
RWTexture3D<uint> outShadowBitmask : register(u2);
RWTexture3D<float> outShadowVisibility : register(u3);
#endif

SamplerComparisonState comparisonSampler : register(s0);

#if !defined(OCCLUSION_ONLY)
static const float3 noise3D[32] = {
	float3(0.247, -0.583, 0.891),
	float3(-0.672, 0.315, -0.428),
	float3(0.934, 0.762, -0.153),
	float3(-0.391, -0.847, 0.526),
	float3(0.618, 0.094, 0.739),
	float3(-0.825, -0.271, -0.683),
	float3(0.152, 0.968, 0.347),
	float3(0.503, -0.714, -0.592),
	float3(-0.436, 0.629, 0.814),
	float3(0.887, -0.198, 0.461),
	float3(-0.759, 0.852, -0.305),
	float3(0.321, -0.476, -0.921),
	float3(-0.094, 0.543, -0.768),
	float3(0.776, 0.418, 0.632),
	float3(-0.538, -0.695, 0.279),
	float3(0.649, -0.921, 0.186),
	float3(-0.913, 0.127, 0.574),
	float3(0.285, 0.806, -0.447),
	float3(0.471, -0.352, 0.698),
	float3(-0.627, -0.194, -0.856),
	float3(0.834, 0.591, -0.712),
	float3(-0.173, -0.968, -0.421),
	float3(0.562, 0.239, -0.785),
	float3(-0.745, 0.487, 0.316),
	float3(0.108, -0.631, 0.894),
	float3(0.926, -0.845, -0.267),
	float3(-0.384, 0.712, -0.539),
	float3(0.697, 0.163, 0.825),
	float3(-0.851, -0.429, 0.641),
	float3(0.214, 0.934, 0.372),
	float3(0.578, -0.762, -0.614),
	float3(-0.469, 0.381, 0.947)
};
#endif

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const float fadeInThreshold = 15;
	const static sh2 unitSH = Skylighting::UNIT_SH;
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;
	const uint3 arrayDims = Skylighting::GetArrayDims();
	uint3 probeTexID = uint3(dtid.xy, settings.SliceStart + dtid.z);
	if (dtid.z >= settings.SliceCount || any(probeTexID >= arrayDims))
		return;
	int3 cellID = (int3(probeTexID) - int3(settings.ArrayOrigin.xyz)) % int3(arrayDims);
	cellID = (cellID + int3(arrayDims)) % int3(arrayDims);
	int3 validMin = max(0, settings.ValidMargin.xyz);
	int3 validMax = int3(arrayDims) - 1 + min(0, settings.ValidMargin.xyz);
	bool isValid = all(cellID >= validMin) && all(cellID <= validMax);  // check if the cell is newly added
	uint probeUpdateState = isValid ? outAccumFramesArray[probeTexID] : 0;
	uint storedAccumFrames = probeUpdateState & 0xFFu;
	uint shadowSampleIndex = (probeUpdateState >> 8) & 31u;
	float3 cellCentreMS = float3(cellID) + 0.5 - float3(arrayDims) * 0.5;
	cellCentreMS = cellCentreMS / float3(arrayDims) * Skylighting::ARRAY_SIZE + settings.PosOffset.xyz;

	float3 cellCentreOS = mul(settings.OcclusionViewProj, float4(cellCentreMS, 1)).xyz;
	cellCentreOS.y = -cellCentreOS.y;
	float2 occlusionUV = cellCentreOS.xy * 0.5 + 0.5;

	if (all(occlusionUV > 0) && all(occlusionUV < 1)) {
		uint accumFrames = storedAccumFrames + 1;
		float visibility = srcOcclusionDepth.SampleCmpLevelZero(comparisonSampler, occlusionUV, cellCentreOS.z);

		sh2 occlusionSH = SphericalHarmonics::Scale(SphericalHarmonics::Evaluate(settings.OcclusionDir.xyz), visibility * 4.0 * Math::PI);  // 4 pi from monte carlo
		if (isValid) {
			float lerpFactor = rcp(accumFrames);
			sh2 prevProbeSH = unitSH;
			if (accumFrames > 1)
				prevProbeSH += (outProbeArray[probeTexID] - unitSH) * fadeInThreshold / min(fadeInThreshold, accumFrames - 1);  // inverse confidence
			occlusionSH = lerp(prevProbeSH, occlusionSH, lerpFactor);
		}
		occlusionSH = lerp(unitSH, occlusionSH, min(fadeInThreshold, accumFrames) / fadeInThreshold);  // confidence fade in

		outProbeArray[probeTexID] = occlusionSH;
		storedAccumFrames = min(accumFrames, 255u);
	} else if (!isValid) {
		outProbeArray[probeTexID] = unitSH;
	}

	// Shadow cascade sampling with bitmask accumulation
#if !defined(OCCLUSION_ONLY)
	// Mono dispatch shared by both eyes (see Skylighting::Prepass); eye index is
	// arbitrary but harmless for this coarse world-space visibility test.
	float4 cellCentreCS = mul(FrameBuffer::CameraViewProj[0], float4(cellCentreMS, 1));
	float2 screenUV = (cellCentreCS.xy / cellCentreCS.w) * float2(0.5, -0.5) + 0.5;
	bool onScreen = cellCentreCS.w > 0 && all(screenUV > 0) && all(screenUV < 1);

	if (onScreen) {
		float shadowSample = 1.0;
		DirectionalShadowLightData shadowData = DirectionalShadowLights[0];

		float3 jitteredMS = cellCentreMS + noise3D[shadowSampleIndex] * 128;
		shadowSampleIndex = (shadowSampleIndex + 1u) & 31u;

		float ndcDepth = FrameBuffer::GetShadowDepth(jitteredMS, 0);
		float linearDepth = SharedData::GetScreenDepth(ndcDepth);

		if (linearDepth > 0 && linearDepth < shadowData.EndSplitDistances.y) {
			float3 positionWS = jitteredMS + FrameBuffer::CameraPosAdjust[0].xyz;

			uint cascadeIndex = (linearDepth > shadowData.EndSplitDistances.x) ? 1u : 0u;

			float3 positionLS = mul(shadowData.ShadowProj[cascadeIndex], float4(positionWS, 1)).xyz;

			positionLS.xy = saturate(positionLS.xy);

			shadowSample = ESRAMShadow.SampleCmpLevelZero(comparisonSampler, float3(positionLS.xy, cascadeIndex), positionLS.z);

			float fade = saturate(linearDepth / shadowData.EndSplitDistances.y);
			float fadeFactor = 1.0 - pow(fade * fade, 8);
			shadowSample = lerp(1.0, shadowSample, fadeFactor);
		}

		uint bitmask = isValid ? outShadowBitmask[probeTexID] : 0;
		bitmask = (bitmask << 1) | (shadowSample > 0.5 ? 1u : 0u);

		outShadowBitmask[probeTexID] = bitmask;

		float shadow = float(countbits(bitmask)) / 32.0;
		outShadowVisibility[probeTexID] = shadow;
	} else if (!isValid) {
		outShadowBitmask[probeTexID] = 0;
		outShadowVisibility[probeTexID] = 1.0;
	}
#endif
	outAccumFramesArray[probeTexID] = storedAccumFrames | (shadowSampleIndex << 8);
}
