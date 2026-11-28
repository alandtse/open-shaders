#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/ReverseZ.hlsli"
#include "Common/SharedData.hlsli"
#include "NRD/Eye.hlsli"
#include "NRD/NRDReblurSH.hlsli"

Texture2D<float> srcDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);

RWTexture2D<float> outViewZ : register(u0);
RWTexture2D<float4> outNormalRoughness : register(u1);

float ScreenToViewDepth(float screenDepth)
{
	if (screenDepth == FrameBuffer::FarPlaneDepth())
		return 3.402823466e+38;
#ifdef VR
	if (screenDepth == FrameBuffer::NearPlaneDepth())
		return 3.402823466e+38;
#endif
	return SharedData::GetScreenDepth(screenDepth);
}

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	if (any(dtid >= uint2(NRDPortActiveWidth, NRDPortActiveHeight)))
		return;

	float depth = srcDepth[dtid];
	float viewZ = ScreenToViewDepth(depth);

	outViewZ[dtid] = viewZ;

	float4 normalGloss = srcNormalRoughness[dtid];
	float3 normalVS = GBuffer::DecodeNormal(normalGloss.xy);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[NRDPortEye], float4(normalVS, 0)).xyz);

	float roughness = 1.0 - normalGloss.z;

	outNormalRoughness[dtid] = NRD_FrontEnd_PackNormalAndRoughness(normalWS, roughness, 0.0);
}
