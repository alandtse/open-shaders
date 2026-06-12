#include "Common/FrameBuffer.hlsli"
#include "Common/LodLandscape.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"
#include "Common/VR.hlsli"

#if defined(RENDER_SHADOWMASK) || defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB) || defined(RENDER_SHADOWMASKDPB)
#	define RENDER_SHADOWMASK_ANY
#endif

struct VS_INPUT
{
	float4 PositionMS: POSITION0;

#if defined(TEXTURE)
	float2 TexCoord: TEXCOORD0;
#endif

#if defined(NORMALS)
	float4 Normal: NORMAL0;
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
	float4 PositionCS: SV_POSITION0;

#if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#	if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 TexCoord0: TEXCOORD0;
#	endif

#	if defined(RENDER_NORMAL)
	float4 Normal: TEXCOORD1;
#	endif

#	if defined(RENDER_SHADOWMAP_PB)
	float3 TexCoord1: TEXCOORD2;
#	elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 TexCoord1: TEXCOORD2;
#	elif defined(ADDITIONAL_ALPHA_MASK)
	float2 TexCoord1: TEXCOORD2;
#	endif

#	if defined(LOCALMAP_FOGOFWAR)
	float Alpha: TEXCOORD3;
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	float4 PositionMS: TEXCOORD5;
#	endif

#	if defined(ALPHA_TEST) && defined(VC) && defined(RENDER_SHADOWMASK_ANY)
	float2 Alpha: TEXCOORD4;
#	elif (defined(ALPHA_TEST) && defined(VC) && !defined(TREE_ANIM)) || defined(RENDER_SHADOWMASK_ANY)
	float Alpha: TEXCOORD4;
#	endif

#	if defined(DEBUG_SHADOWSPLIT)
	float Depth: TEXCOORD2;
#	endif
#endif
#if defined(VR)
	float ClipDistance: SV_ClipDistance0;  // o11
	float CullDistance: SV_CullDistance0;  // p11
	uint EyeIndex: EYEIDX0;
#endif  // VR
};

#ifdef VSHADER
#	include "UtilityVS.hlsli"
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#ifdef PSHADER
#	include "UtilityPS.hlsli"
#endif
