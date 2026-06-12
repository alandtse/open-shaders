#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#define DEFERRED

#ifdef GRASS_LIGHTING
#	define GRASS
#endif  // GRASS_LIGHTING

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
	float4 Normal: NORMAL0;
	float4 Color: COLOR0;
	float4 InstanceData1: TEXCOORD4;
	float4 InstanceData2: TEXCOORD5;
	float4 InstanceData3: TEXCOORD6;
	float4 InstanceData4: TEXCOORD7;
#ifdef VR
	uint InstanceID: SV_INSTANCEID;
#endif  // VR
};

#ifdef GRASS_LIGHTING
struct VS_OUTPUT
{
	float4 HPosition: SV_POSITION0;
	float4 Color: COLOR0;
	float VertexMult: COLOR1;
	float3 TexCoord: TEXCOORD0;
	float3 ViewSpacePosition:
#	if !defined(VR)
		TEXCOORD1;
#	else
		TEXCOORD2;
#	endif
#	if defined(RENDER_DEPTH)
	float2 Depth:
#		if !defined(VR)
		TEXCOORD2;
#		else
		TEXCOORD3;
#		endif
#	endif  // RENDER_DEPTH
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float4 VertexNormal: POSITION4;
#	ifdef VR
	float ClipDistance: SV_ClipDistance0;
	float CullDistance: SV_CullDistance0;
#	endif  // VR
};
#else
struct VS_OUTPUT
{
	float4 HPosition: SV_POSITION0;
	float4 Color: COLOR0;
	float VertexMult: COLOR1;
	float3 TexCoord: TEXCOORD0;
	float4 AmbientColor: TEXCOORD1;
	float3 ViewSpacePosition: TEXCOORD2;
#	if defined(RENDER_DEPTH)
	float2 Depth: TEXCOORD3;
#	endif  // RENDER_DEPTH
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
#	ifdef VR
	float ClipDistance: SV_ClipDistance0;
	float CullDistance: SV_CullDistance0;
#	endif  // VR
};
#endif

// Constant Buffers (Flat and VR)
cbuffer PerGeometry : register(
#ifdef VSHADER
						  b2
#else
						  b3
#endif
					  )
{
#if !defined(VR)
	row_major float4x4 WorldViewProj[1] : packoffset(c0);
	row_major float4x4 WorldView[1] : packoffset(c4);
	row_major float4x4 World[1] : packoffset(c8);
	row_major float4x4 PreviousWorld[1] : packoffset(c12);
	float4 FogNearColor : packoffset(c16);
	float3 WindVector : packoffset(c17);
	float WindTimer : packoffset(c17.w);
	float3 DirLightDirection : packoffset(c18);
	float PreviousWindTimer : packoffset(c18.w);
	float3 DirLightColor : packoffset(c19);
	float AlphaParam1 : packoffset(c19.w);
	float3 AmbientColor : packoffset(c20);
	float AlphaParam2 : packoffset(c20.w);
	float3 ScaleMask : packoffset(c21);
	float ShadowClampValue : packoffset(c21.w);
#else
	row_major float4x4 WorldViewProj[2] : packoffset(c0);
	row_major float4x4 WorldView[2] : packoffset(c8);
	row_major float4x4 World[2] : packoffset(c16);
	row_major float4x4 PreviousWorld[2] : packoffset(c24);
	float4 FogNearColor : packoffset(c32);
	float3 WindVector : packoffset(c33);
	float WindTimer : packoffset(c33.w);
	float3 DirLightDirection : packoffset(c34);
	float PreviousWindTimer : packoffset(c34.w);
	float3 DirLightColor : packoffset(c35);
	float AlphaParam1 : packoffset(c35.w);
	float3 AmbientColor : packoffset(c36);
	float AlphaParam2 : packoffset(c36.w);
	float3 ScaleMask : packoffset(c37);
	float ShadowClampValue : packoffset(c37.w);
#endif  // !VR
}

#ifdef VSHADER
#	include "RunGrassVS.hlsli"
#endif  // VSHADER

typedef VS_OUTPUT PS_INPUT;

#ifdef GRASS_LIGHTING
struct PS_OUTPUT
{
#	if defined(RENDER_DEPTH)
	float4 PS: SV_Target0;
#	else
	float4 Diffuse: SV_Target0;
	float2 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
#		if defined(TRUE_PBR)
	float4 Reflectance: SV_Target5;
#		endif  // TRUE_PBR
	float4 Masks: SV_Target6;
	float4 Masks2: SV_Target7;
#	endif      // RENDER_DEPTH
};
#else
struct PS_OUTPUT
{
#	if defined(RENDER_DEPTH)
	float4 PS: SV_Target0;
#	else
	float4 Diffuse: SV_Target0;
	float2 MotionVectors: SV_Target1;
	float4 Normal: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Masks: SV_Target6;
	float4 Masks2: SV_Target7;
#	endif
};
#endif

#ifdef PSHADER
#	include "RunGrassPS.hlsli"
#endif  // PSHADER
