#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Skylighting/Skylighting.hlsli"

Texture2D<unorm float> AllGeometryDepth : register(t0);
Texture2D<unorm float> SolidCoverDepth : register(t1);
RWTexture3D<float> CanopyClassification : register(u0);
RWTexture3D<uint> CanopyAccumulation : register(u1);
SamplerComparisonState CoverSampler : register(s0);

[numthreads(8, 8, 1)] void RainCanopyUpdateCS(uint3 dispatchThreadID : SV_DispatchThreadID) {
	static const uint convergenceFrames = 15u;
	const SharedData::SkylightingSettings settings = SharedData::skylightingSettings;
	uint3 cellID = uint3(max(int3(dispatchThreadID) - settings.ArrayOrigin.xyz, 0) % Skylighting::ARRAY_DIM);
	uint3 validMinimum = uint3(max(0, settings.ValidMargin.xyz));
	uint3 validMaximum = Skylighting::ARRAY_DIM - 1u + uint3(min(0, settings.ValidMargin.xyz));
	bool isValid = all(cellID >= validMinimum) && all(cellID <= validMaximum);

	float3 cellCentreMS = cellID + 0.5f - Skylighting::ARRAY_DIM * 0.5f;
	cellCentreMS = cellCentreMS / Skylighting::ARRAY_DIM * Skylighting::ARRAY_SIZE + settings.PosOffset.xyz;
	float3 cellCentreOS = mul(settings.OcclusionViewProj, float4(cellCentreMS, 1.0f)).xyz;
	cellCentreOS.y = -cellCentreOS.y;
	float2 occlusionUV = cellCentreOS.xy * 0.5f + 0.5f;

	if (all(occlusionUV > 0.0f) && all(occlusionUV < 1.0f)) {
		uint accumulatedFrames = isValid ? min(CanopyAccumulation[dispatchThreadID] + 1u, 255u) : 1u;
		float allGeometryVisibility = AllGeometryDepth.SampleCmpLevelZero(CoverSampler, occlusionUV, cellCentreOS.z);
		float solidCoverVisibility = SolidCoverDepth.SampleCmpLevelZero(CoverSampler, occlusionUV, cellCentreOS.z);
		float canopy = saturate(solidCoverVisibility - allGeometryVisibility);
		if (isValid && accumulatedFrames > 1u) {
			float sampleWeight = rcp(min(float(accumulatedFrames), float(convergenceFrames)));
			canopy = lerp(CanopyClassification[dispatchThreadID], canopy, sampleWeight);
		}
		CanopyClassification[dispatchThreadID] = canopy;
		CanopyAccumulation[dispatchThreadID] = accumulatedFrames;
	} else if (!isValid) {
		CanopyClassification[dispatchThreadID] = 0.0f;
		CanopyAccumulation[dispatchThreadID] = 0u;
	}
}
