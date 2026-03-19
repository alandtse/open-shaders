#include "Common/Math.hlsli"
#include "Skylighting/Skylighting.hlsli"

Texture2D<unorm float> srcOcclusionDepth : register(t0);

RWTexture3D<sh2> outProbeArray : register(u0);
RWTexture3D<uint> outAccumFramesArray : register(u1);

SamplerComparisonState comparisonSampler : register(s0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const float fadeInThreshold = 15;
	const static sh2 unitSH = float4(sqrt(4.0 * Math::PI), 0, 0, 0);
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;
	const uint3 arrayDims = max(settings.ArrayDims.xyz, uint3(1, 1, 1));
	const float3 arrayDimsF = float3(arrayDims);
	uint sliceCount = max(1u, settings.ProbeUpdateSliceCount);
	uint probeSlice = settings.ProbeUpdateSliceStart + dtid.z;
	if (dtid.z >= sliceCount || probeSlice >= arrayDims.z)
		return;

	uint3 probeTexID = uint3(dtid.xy, probeSlice);
	int3 cellIDInt = int3(probeTexID) - int3(settings.ArrayOrigin.xyz);
	cellIDInt = (cellIDInt % int3(arrayDims) + int3(arrayDims)) % int3(arrayDims);
	uint3 cellID = uint3(cellIDInt);
	int3 validMin = max(0, settings.ValidMargin.xyz);
	int3 validMax = int3(arrayDims) - 1 + min(0, settings.ValidMargin.xyz);
	bool isValid = all(cellIDInt >= validMin) && all(cellIDInt <= validMax);  // check if the cell is newly added
	float3 cellCentreMS = float3(cellID) + 0.5 - arrayDimsF * 0.5;
	cellCentreMS = cellCentreMS / arrayDimsF * Skylighting::ARRAY_SIZE + settings.PosOffset.xyz;

	float3 cellCentreOS = mul(settings.OcclusionViewProj, float4(cellCentreMS, 1)).xyz;
	cellCentreOS.y = -cellCentreOS.y;
	float2 occlusionUV = cellCentreOS.xy * 0.5 + 0.5;

	if (all(occlusionUV > 0) && all(occlusionUV < 1)) {
		uint accumFrames = isValid ? (outAccumFramesArray[probeTexID] + 1) : 1;
		float visibility = srcOcclusionDepth.SampleCmpLevelZero(comparisonSampler, occlusionUV, cellCentreOS.z);

		sh2 occlusionSH = settings.OcclusionSHBasis4Pi * visibility;
		if (isValid) {
			float lerpFactor = rcp(accumFrames);
			sh2 prevProbeSH = unitSH;
			if (accumFrames > 1)
				prevProbeSH += (outProbeArray[probeTexID] - unitSH) * fadeInThreshold / min(fadeInThreshold, accumFrames - 1);  // inverse confidence
			occlusionSH = lerp(prevProbeSH, occlusionSH, lerpFactor);
		}
		occlusionSH = lerp(unitSH, occlusionSH, min(fadeInThreshold, accumFrames) / fadeInThreshold);  // confidence fade in

		outProbeArray[probeTexID] = occlusionSH;
		outAccumFramesArray[probeTexID] = accumFrames;
	} else if (!isValid) {
		outProbeArray[probeTexID] = unitSH;
		outAccumFramesArray[probeTexID] = 0;
	}
}
