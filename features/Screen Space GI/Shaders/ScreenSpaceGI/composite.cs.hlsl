#include "NRD/Eye.hlsli"
#include "Common/ReverseZ.hlsli"
#include "NRD/OpenShadersRadiance.hlsli"
#include "Common/SharedData.hlsli"
#include "NRD/NRDReblurSH.hlsli"

Texture2D<unorm float3> AlbedoTexture : register(t0);
Texture2D<unorm float3> NormalTexture : register(t1);
#include "Common/GBuffer.hlsli"
#if defined(IBL)
#include "IBL/IBL.hlsli"
#endif
#if defined(SKYLIGHTING)
#define SKYLIGHTING_PROBE_REGISTER t6
#define SKYLIGHTING_COVERAGE_REGISTER t18
#define SKYLIGHTING_ZENITH_REGISTER t19
#include "Skylighting/Skylighting.hlsli"
#endif
// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT (no unorm),
// OFF -> R24_UNORM_X8_TYPELESS game depth (unorm).
Texture2D<float> DepthTexture : register(t2);

Texture2D<float4> SsgiTexture : register(t3);
#if defined(SSGI_SH)
Texture2D<float4> SsgiSH1Texture : register(t4);
#endif
Texture2D<float3> MasksTexture : register(t5);

RWTexture2D<float4> MainRW : register(u0);

float3 SampleSSGIIL(uint2 pixCoord)
{
#if defined(SSGI_SH)
	NRD_SG sg = REBLUR_BackEnd_UnpackSh(SsgiTexture[pixCoord], SsgiSH1Texture[pixCoord]);
	// The tracer stores cosine-convolved diffuse illumination in SH0. SH1 is a
	// directional moment for denoising, not an incident lobe to shade again.
	float3 radiance = _NRD_YCoCgToLinear(float3(sg.c0, sg.chroma));
#else
	float normHitDist;
	float3 radiance;
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SsgiTexture[pixCoord], radiance, normHitDist);
#endif
	return max(0, radiance);
}

bool IsSkinMask(float2 mask)
{
	return mask.x > 0.0 && mask.y < 1.5;
}

float3 SampleSSGIILSkin(uint2 pixCoord)
{
	const int2 maxCoord = int2(NRDPortActiveWidth,NRDPortActiveHeight) - 1;
	const float centerDepth = SharedData::GetScreenDepth(DepthTexture[pixCoord]);
	float3 il = 0.0;
	float totalWeight = 0.0;
	[unroll] for (int y = -6; y <= 6; y += 3)
	{
		[unroll] for (int x = -6; x <= 6; x += 3)
		{
			const int2 tap = clamp(int2(pixCoord) + int2(x, y), 0, maxCoord);
			if (!IsSkinMask(MasksTexture[tap].xy))
				continue;
			const float difference = abs(SharedData::GetScreenDepth(DepthTexture[tap]) - centerDepth) / max(centerDepth * 0.01, 1.0);
			const float weight = exp2(-difference * difference);
			il += SampleSSGIIL(tap) * weight;
			totalWeight += weight;
		}
	}
	return il / max(totalWeight, EPSILON_DIVISION);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(NRDPortActiveWidth,NRDPortActiveHeight)) || SharedData::ssgiSettings.EnableIL == 0)
		return;

	float depth = DepthTexture[dispatchID.xy];
	if (abs(depth - FrameBuffer::FarPlaneDepth()) <= 1e-6)
		return;

	float3 linAlbedo = Color::IrradianceToLinear(AlbedoTexture[dispatchID.xy] / Color::PBRLightingScale);
	float3 ssgiIl = IsSkinMask(MasksTexture[dispatchID.xy].xy) ? SampleSSGIILSkin(dispatchID.xy) : SampleSSGIIL(dispatchID.xy);
	ssgiIl = max(lerp(Color::RGBToLuminance(ssgiIl), ssgiIl, NRDPortILSaturation), 0.0);
	// Masks.z is an IL gain here: Lighting's character light fill, or grass transmission albedo relative to reflection albedo.


	float4 mainColor = MainRW[dispatchID.xy + uint2(NRDPortOffsetX,0)];
	float3 linDiffuseColor = NRDPort::SceneToLinear(mainColor.xyz);
	if (NRDPortReplaceAmbient != 0) {
        float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[NRDPortEye], float4(GBuffer::DecodeNormal(NormalTexture[dispatchID.xy].xy),0)).xyz);
        float3 ambient = Color::Ambient(max(0, SharedData::GetAmbient(normalWS)));
#if defined(IBL)
        if (SharedData::iblSettings.EnableIBL) {
#if defined(SKYLIGHTING)
            float2 uv = (dispatchID.xy + 0.5) / float2(NRDPortActiveWidth,NRDPortActiveHeight);
            float4 pos = mul(FrameBuffer::CameraViewProjInverse[NRDPortEye], float4(2 * float2(uv.x, 1-uv.y)-1, depth, 1));
            float3 positionMS = pos.xyz / pos.w;
#if defined(VR)
 positionMS += FrameBuffer::CameraPosAdjust[NRDPortEye].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#endif
            float visibility = Skylighting::EvaluateDiffuse(Skylighting::Sample(positionMS,normalWS),normalWS,Skylighting::GetFadeOutFactor(positionMS));
            ambient = ImageBasedLighting::GetDiffuseIBLOccluded(ambient,-normalWS,visibility);
#else
            ambient = ImageBasedLighting::GetDiffuseIBL(ambient,-normalWS);
#endif
        }
#endif
        ambient *= AlbedoTexture[dispatchID.xy];
        float3 yc = Color::RGBToYCoCg(ambient);
        yc.x = MasksTexture[dispatchID.xy].z;
        linDiffuseColor = max(0,linDiffuseColor - Color::IrradianceToLinear(max(0,Color::YCoCgToRGB(yc))));
    }
    linDiffuseColor += ssgiIl * linAlbedo;
	MainRW[dispatchID.xy + uint2(NRDPortOffsetX,0)] = float4(NRDPort::LinearToScene(linDiffuseColor), mainColor.a);
}
