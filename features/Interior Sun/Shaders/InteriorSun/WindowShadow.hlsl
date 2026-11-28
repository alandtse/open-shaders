#include "Common/WindowShadow.hlsli"

cbuffer Pane : register(b0)
{
	float AlphaReference;
	float3 Padding;
	row_major float4x4 WorldViewProj;
	float4 UVTransform;
};

Texture2D<float> EntryTexture : register(t0);

struct PaneVertex
{
	float4 position: SV_Position;
	float4 texCoord: TEXCOORD2;
	float clipDistance: SV_ClipDistance0;
#ifdef VR
	float cullDistance: SV_CullDistance0;
	uint eyeIndex: EYEIDX0;
#endif
};

PaneVertex PaneVS(float3 position : POSITION, float2 uv : TEXCOORD0)
{
	PaneVertex output;
	output.position = mul(float4(position, 1), WorldViewProj);
	output.texCoord = float4(output.position.zw, uv * UVTransform.xy + UVTransform.zw);
	// A pane sunward of the cascade still illuminates receivers inside it.
	output.clipDistance = output.position.w - output.position.z;
	output.position.z = 0;
#ifdef VR
	output.cullDistance = 1;
	output.eyeIndex = 0;
#endif
	return output;
}

float4 FullscreenVS(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1u) & 2u, id & 2u);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float InitializeDepthPS(float4 position : SV_Position) : SV_Depth
{
	return WindowShadow::Open(EntryTexture.Load(int3(int2(position.xy), 0))) ? 1.0 : 0.0;
}

void OpaqueCasterPS(float4 position : SV_Position)
{
	WindowShadow::ClipSunwardCaster(position.xy, position.z);
}
