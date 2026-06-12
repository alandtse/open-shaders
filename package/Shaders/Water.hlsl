#if defined(UNDERWATERMASK)

struct VS_INPUT
{
	float4 Position: POSITION0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
};

#	ifdef VSHADER

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float z = min(1, 1e-4 * max(0, input.Position.z - 70000)) * 0.5 + input.Position.z;
	vsout.Position = float4(input.Position.xy, z, 1);

	return vsout;
}
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#	ifdef PSHADER
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	psout.Color = 1;

	return psout;
}
#	endif

#else

#	include "Common/FrameBuffer.hlsli"
#	include "Common/MotionBlur.hlsli"
#	include "Common/Permutation.hlsli"
#	include "Common/Random.hlsli"
#	include "Common/Color.hlsli"

#	define WATER

#	include "Common/SharedData.hlsli"

struct VS_INPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER) || defined(STENCIL) || defined(SIMPLE)
	float4 Position: POSITION0;
#		if defined(NORMAL_TEXCOORD)
	float2 TexCoord0: TEXCOORD0;
#		endif
#		if defined(VC)
	float4 Color: COLOR0;
#		endif
#	endif

#	if defined(LOD)
	float4 Position: POSITION0;
#		if defined(VC)
	float4 Color: COLOR0;
#		endif
#	endif
#	if defined(VR)
	uint InstanceID: SV_INSTANCEID;
#	endif  // VR
};

struct VS_OUTPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER)
	float4 HPosition: SV_POSITION0;
#		if !defined(UNIFIED_WATER)
	float4 FogParam: COLOR0;
#		endif
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
#		if defined(WADING) || (defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS))) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)) || ((defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0) && defined(FLOWMAP) /*!defined(NORMAL_TEXCOORD) && !defined(BLEND_NORMALS) && !defined(VC)*/)
	float4 TexCoord3: TEXCOORD3;
#		endif
#		if defined(FLOWMAP)
	nointerpolation float2 TexCoord4: TEXCOORD4;
#		endif
#		if NUM_SPECULAR_LIGHTS == 0
	float4 MPosition: TEXCOORD5;
#		endif
#	endif

#	if defined(SIMPLE)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
	float4 MPosition: TEXCOORD5;
#	endif

#	if defined(LOD)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
#	endif

#	if defined(STENCIL)
	float4 HPosition: SV_POSITION0;
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
#	endif

	float4 NormalsScale: TEXCOORD8;
#	if defined(VR)
	float ClipDistance: SV_ClipDistance0;  // o11
	float CullDistance: SV_CullDistance0;  // p11
#	endif  // VR
};

#	ifdef VSHADER
#		include "WaterVS.hlsli"
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#	if defined(UNDERWATER) || defined(SIMPLE) || defined(LOD) || defined(SPECULAR)
	float4 Lighting: SV_Target0;
#	endif

#	if defined(STENCIL)
	float4 WaterMask: SV_Target0;
	float2 MotionVector: SV_Target1;
#	endif
};

#	ifdef PSHADER
#		include "WaterPS.hlsli"
#	endif

#endif
