#define LIGHTING

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/LodLandscape.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"
#include "Common/Triplanar.hlsli"
#include "Common/VR.hlsli"

#if defined(FACEGEN) || defined(FACEGEN_RGB_TINT)
#	define SKIN
#endif

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if (defined(TREE_ANIM) || defined(LANDSCAPE)) && !defined(VC)
#	define VC
#endif  // TREE_ANIM || LANDSCAPE || !VC

#if defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(LODLANDNOISE) || defined(WORLD_MAP)
#	define LOD
#endif

struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord0: TEXCOORD0;
#if !defined(MODELSPACENORMALS)
	float4 Normal: NORMAL0;
	float4 Bitangent: BINORMAL0;
#endif  // !MODELSPACENORMALS

#if defined(VC)
	float4 Color: COLOR0;
#	if defined(LANDSCAPE)
	float4 LandBlendWeights1: TEXCOORD2;
	float4 LandBlendWeights2: TEXCOORD3;
#	endif  // LANDSCAPE
#endif      // VC
#if defined(SKINNED)
	float4 BoneWeights: BLENDWEIGHT0;
	float4 BoneIndices: BLENDINDICES0;
#endif  // SKINNED
#if defined(EYE)
	float EyeParameter: TEXCOORD2;
#endif  // EYE
#if defined(VR)
	uint InstanceID: SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
#if (defined(PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
	float4
#else
	float2
#endif  // (defined (PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
		TexCoord0: TEXCOORD0;

#if defined(WORLD_MAP)
	float3 InputPosition: TEXCOORD4;
#endif

#if defined(SKINNED) || !defined(MODELSPACENORMALS)
	float3 TBN0: TEXCOORD1;
	float3 TBN1: TEXCOORD2;
	float3 TBN2: TEXCOORD3;
#endif  // defined(SKINNED) || !defined(MODELSPACENORMALS)
#if defined(EYE)
	float3 EyeNormal: TEXCOORD6;
#elif defined(LANDSCAPE)
	float4 LandBlendWeights1: TEXCOORD6;
	float4 LandBlendWeights2: TEXCOORD7;
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	float3 TexProj: TEXCOORD7;
#endif  // EYE

	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float4 Color: COLOR0;
	float4 FogParam: COLOR1;

#if defined(VR)
	float ClipDistance: SV_ClipDistance0;  // o11
	float CullDistance: SV_CullDistance0;  // p11
#endif

	float3 ModelPosition: TEXCOORD12;
};
#ifdef VSHADER
#	include "LightingVS.hlsli"
#endif  // VSHADER

typedef VS_OUTPUT PS_INPUT;

#if !defined(LANDSCAPE)
#	undef TERRAIN_BLENDING
#endif

#if defined(DEFERRED)
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
	float4 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
	float4 Reflectance: SV_Target5;
	float4 Masks: SV_Target6;
	float4 Masks2: SV_Target7;
};
#else
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
	float4 MotionVectors: SV_Target1;
};
#endif

#ifdef PSHADER
#	include "LightingPS.hlsli"
#endif  // PSHADER
