#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/ReverseZ.hlsli"
#include "NRD/Eye.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<float4> srcNormalRoughness : register(t0);
Texture2D<float> srcWorkingDepth : register(t1);

RWTexture2D<unorm float2> outNormal0 : register(u0);
RWTexture2D<unorm float2> outNormal1 : register(u1);
RWTexture2D<unorm float2> outNormal2 : register(u2);
RWTexture2D<unorm float2> outNormal3 : register(u3);
RWTexture2D<unorm float2> outNormal4 : register(u4);
RWTexture2D<half3> outCurrentGeo : register(u5);

float2 NormalMIPFilter(float2 enc0, float2 enc1, float2 enc2, float2 enc3, float4 valid)
{
	float3 avg = GBuffer::DecodeNormal(enc0) * valid.x + GBuffer::DecodeNormal(enc1) * valid.y + GBuffer::DecodeNormal(enc2) * valid.z + GBuffer::DecodeNormal(enc3) * valid.w;
	return dot(avg, avg) > 1e-12 ? GBuffer::EncodeNormal(normalize(avg)) : GBuffer::EncodeNormal(float3(0, 0, 1));
}

void WriteCurrentGeometry(uint2 pixCoord, float2 encodedNormal)
{
	if (any(pixCoord >= uint2(OUT_FRAME_DIM)))
		return;

	float viewZ = READ_DEPTH(srcWorkingDepth, pixCoord);
	if (viewZ <= FP_Z) {
		outCurrentGeo[pixCoord] = 0;
		return;
	}

	float3 normalVS = GBuffer::DecodeNormal(encodedNormal);
	float3 normalWS = normalize(ViewToWorldVector(normalVS, FrameBuffer::CameraViewInverse[NRDPortEye]));
	outCurrentGeo[pixCoord] = half3(viewZ, GBuffer::EncodeNormal(normalWS));
}

groupshared float2 g_scratchNormal[8][8];
groupshared float g_scratchValid[8][8];
[numthreads(8, 8, 1)] void main(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID) {
	// MIP 0
	const uint2 baseCoord = dispatchThreadID;
	const uint2 pixCoord = baseCoord * 2;
	const uint2 active = uint2(OUT_FRAME_DIM);
	float4 valid;
	const uint2 p0 = pixCoord + uint2(0, 0);
	valid[0] = all(p0 < active) && srcWorkingDepth[min(p0, active - 1u)] > 0.0 ? 1.0 : 0.0;
	float2 normal0 = valid[0] > 0.0 ? srcNormalRoughness[min(p0, active - 1u)].xy : GBuffer::EncodeNormal(float3(0, 0, 1));
	const uint2 p1 = pixCoord + uint2(1, 0);
	valid[1] = all(p1 < active) && srcWorkingDepth[min(p1, active - 1u)] > 0.0 ? 1.0 : 0.0;
	float2 normal1 = valid[1] > 0.0 ? srcNormalRoughness[min(p1, active - 1u)].xy : GBuffer::EncodeNormal(float3(0, 0, 1));
	const uint2 p2 = pixCoord + uint2(0, 1);
	valid[2] = all(p2 < active) && srcWorkingDepth[min(p2, active - 1u)] > 0.0 ? 1.0 : 0.0;
	float2 normal2 = valid[2] > 0.0 ? srcNormalRoughness[min(p2, active - 1u)].xy : GBuffer::EncodeNormal(float3(0, 0, 1));
	const uint2 p3 = pixCoord + uint2(1, 1);
	valid[3] = all(p3 < active) && srcWorkingDepth[min(p3, active - 1u)] > 0.0 ? 1.0 : 0.0;
	float2 normal3 = valid[3] > 0.0 ? srcNormalRoughness[min(p3, active - 1u)].xy : GBuffer::EncodeNormal(float3(0, 0, 1));

	outNormal0[pixCoord + uint2(0, 0)] = normal0;
	outNormal0[pixCoord + uint2(1, 0)] = normal1;
	outNormal0[pixCoord + uint2(0, 1)] = normal2;
	outNormal0[pixCoord + uint2(1, 1)] = normal3;
	[branch] if (MultiBounceMode != 0)
	{
		WriteCurrentGeometry(pixCoord + uint2(0, 0), normal0);
		WriteCurrentGeometry(pixCoord + uint2(1, 0), normal1);
		WriteCurrentGeometry(pixCoord + uint2(0, 1), normal2);
		WriteCurrentGeometry(pixCoord + uint2(1, 1), normal3);
	}

	// MIP 1
	float2 nm1 = NormalMIPFilter(normal0, normal1, normal2, normal3, valid);
	outNormal1[baseCoord] = nm1;
	g_scratchNormal[groupThreadID.x][groupThreadID.y] = nm1;
	g_scratchValid[groupThreadID.x][groupThreadID.y] = any(valid > 0.0) ? 1.0 : 0.0;

	GroupMemoryBarrierWithGroupSync();

	// MIP 2
	[branch] if (all((groupThreadID.xy % 2) == 0))
	{
		float2 inTL = g_scratchNormal[groupThreadID.x + 0][groupThreadID.y + 0];
		float2 inTR = g_scratchNormal[groupThreadID.x + 1][groupThreadID.y + 0];
		float2 inBL = g_scratchNormal[groupThreadID.x + 0][groupThreadID.y + 1];
		float2 inBR = g_scratchNormal[groupThreadID.x + 1][groupThreadID.y + 1];

		float4 mipValid = float4(g_scratchValid[groupThreadID.x][groupThreadID.y], g_scratchValid[groupThreadID.x + 1][groupThreadID.y],
			g_scratchValid[groupThreadID.x][groupThreadID.y + 1], g_scratchValid[groupThreadID.x + 1][groupThreadID.y + 1]);
		float2 nm2 = NormalMIPFilter(inTL, inTR, inBL, inBR, mipValid);
		outNormal2[baseCoord / 2] = nm2;
		g_scratchNormal[groupThreadID.x][groupThreadID.y] = nm2;
		g_scratchValid[groupThreadID.x][groupThreadID.y] = any(mipValid > 0.0) ? 1.0 : 0.0;
	}

	GroupMemoryBarrierWithGroupSync();

	// MIP 3
	[branch] if (all((groupThreadID.xy % 4) == 0))
	{
		float2 inTL = g_scratchNormal[groupThreadID.x + 0][groupThreadID.y + 0];
		float2 inTR = g_scratchNormal[groupThreadID.x + 2][groupThreadID.y + 0];
		float2 inBL = g_scratchNormal[groupThreadID.x + 0][groupThreadID.y + 2];
		float2 inBR = g_scratchNormal[groupThreadID.x + 2][groupThreadID.y + 2];

		float4 mipValid = float4(g_scratchValid[groupThreadID.x][groupThreadID.y], g_scratchValid[groupThreadID.x + 2][groupThreadID.y],
			g_scratchValid[groupThreadID.x][groupThreadID.y + 2], g_scratchValid[groupThreadID.x + 2][groupThreadID.y + 2]);
		float2 nm3 = NormalMIPFilter(inTL, inTR, inBL, inBR, mipValid);
		outNormal3[baseCoord / 4] = nm3;
		g_scratchNormal[groupThreadID.x][groupThreadID.y] = nm3;
		g_scratchValid[groupThreadID.x][groupThreadID.y] = any(mipValid > 0.0) ? 1.0 : 0.0;
	}

	GroupMemoryBarrierWithGroupSync();

	// MIP 4
	[branch] if (all((groupThreadID.xy % 8) == 0))
	{
		float2 inTL = g_scratchNormal[groupThreadID.x + 0][groupThreadID.y + 0];
		float2 inTR = g_scratchNormal[groupThreadID.x + 4][groupThreadID.y + 0];
		float2 inBL = g_scratchNormal[groupThreadID.x + 0][groupThreadID.y + 4];
		float2 inBR = g_scratchNormal[groupThreadID.x + 4][groupThreadID.y + 4];

		float4 mipValid = float4(g_scratchValid[groupThreadID.x][groupThreadID.y], g_scratchValid[groupThreadID.x + 4][groupThreadID.y],
			g_scratchValid[groupThreadID.x][groupThreadID.y + 4], g_scratchValid[groupThreadID.x + 4][groupThreadID.y + 4]);
		float2 nm4 = NormalMIPFilter(inTL, inTR, inBL, inBR, mipValid);
		outNormal4[baseCoord / 8] = nm4;
	}
}
