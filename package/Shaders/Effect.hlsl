#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"
#include "Common/VR.hlsli"

#define EFFECT

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

struct VS_INPUT
{
	float4 Position: POSITION0;
#if defined(TEXCOORD)
#	if defined(STRIP_PARTICLES)
	float3
#	else
	float2
#	endif
		TexCoord0: TEXCOORD0;
#endif
#if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float4 Normal: NORMAL0;
#endif
#if defined(BINORMAL_TANGENT)
	float4 Bitangent: BINORMAL0;
#endif
#if defined(VC)
	float4 Color: COLOR0;
#endif
#if defined(SKINNED)
	float4 BoneWeights: BLENDWEIGHT0;
	float4 BoneIndices: BLENDINDICES0;
#endif
#if defined(VR)
	uint InstanceID: SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float4 TexCoord0: TEXCOORD0;
	float4 WorldPosition: POSITION1;
#if defined(VC)
	float4 Color: COLOR0;
#endif
#if !defined(MOTIONVECTORS_NORMALS)
	float4 FogParam: COLOR1;
#endif
#if defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS)
	float3 ScreenSpaceNormal: TEXCOORD1;
#elif (defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))) || (defined(PROJECTED_UV) && defined(NORMALS))
	float3 TBN0: TEXCOORD1;
#endif
#if defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))
	float FogAlpha: TEXCOORD5;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS)) || (defined(PROJECTED_UV) && defined(NORMALS) && !defined(MEMBRANE))
	float3 TBN1: TEXCOORD2;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS))
	float3 TBN2: TEXCOORD3;
#endif
#if defined(MEMBRANE)
	float4 ViewVector: TEXCOORD4;
#endif
#if defined(LIGHTING)
	float3 MSPosition: TEXCOORD6;
#endif
#if !(defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS)))
	float FogAlpha: TEXCOORD5;
#endif
#if defined(MOTIONVECTORS_NORMALS)
#	if !defined(LIGHTING) && !(defined(MEMBRANE) && defined(SKINNED)) && !(defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS))
	float3 ScreenSpaceNormal: TEXCOORD7;
#	endif
	float4 PreviousWorldPosition: POSITION2;
#	if (defined(LIGHTING) || (defined(MEMBRANE) && defined(SKINNED))) && !(defined(MEMBRANE) && defined(NORMALS))
	float3 ScreenSpaceNormal: TEXCOORD7;
#	endif
#endif
#if defined(VR)
	float ClipDistance: SV_ClipDistance0;  // o11
	float CullDistance: SV_CullDistance0;  // p11
	uint EyeIndex: EYEIDX0;
#endif  // VR
};

#ifdef VSHADER
#	include "EffectVS.hlsli"
#endif

typedef VS_OUTPUT PS_INPUT;
SamplerState SampBaseSampler : register(s0);
SamplerState SampNormalSampler : register(s1);
SamplerState SampNoiseSampler : register(s2);
SamplerState SampDepthSampler : register(s3);
SamplerState SampGrayscaleSampler : register(s4);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexNormalSampler : register(t1);
Texture2D<float4> TexNoiseSampler : register(t2);
Texture2D<float4> TexDepthSamplerEffect : register(t3);
Texture2D<float4> TexGrayscaleSampler : register(t4);

#if defined(DEFERRED)
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
#	if defined(MOTIONVECTORS_NORMALS)
	float4 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
#	elif defined(NORMALS)
	float4 NormalGlossiness: SV_Target2;
#	endif
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
	float4 Reflectance: SV_Target5;
	float4 Masks: SV_Target6;
};
#else
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
#	if defined(MOTIONVECTORS_NORMALS)
	float2 MotionVectors: SV_Target1;
	float4 ScreenSpaceNormals: SV_Target2;
#	else
	float4 Color2: SV_Target2;
#	endif
};
#endif

#ifdef PSHADER
#	include "EffectPS.hlsli"
#endif
