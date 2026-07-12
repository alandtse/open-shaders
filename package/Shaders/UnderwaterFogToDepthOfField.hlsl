#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float2 TexCoord: TEXCOORD0;
};

#if defined(VSHADER)
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT output;

	float2 texCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(texCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	output.TexCoord = texCoord;

	return output;
}
#endif

#if defined(PSHADER)
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

Texture2D<float4> SourceTex : register(t0);
Texture2D<float> DepthTex : register(t1);
Texture2D<float> MaskTex : register(t2);

cbuffer PerPass : register(b0)
{
	float4 FogColor;
	float4 FogRange;
	float4 DofParams;
	float4 DofParams3;
	float4 DofParams6;
	float4 UnderwaterDepthOfFieldFlags;
};

float GetResolvedFinalDepth(float rawDepth, float mask)
{
	if (rawDepth <= 1e-5)
		return rawDepth;

	float depth;
	float nearPlane;
	float farPlane;
	if (rawDepth <= 0.01) {
		depth = 100.0 * rawDepth;
		nearPlane = DofParams3.x;
		farPlane = DofParams3.y;
	} else {
		depth = 1.01 * rawDepth - 0.01;
		float4 dofParams = UnderwaterDepthOfFieldFlags.x != 0.0 ? lerp(DofParams, DofParams6, mask) : DofParams;
		nearPlane = dofParams.z;
		farPlane = dofParams.w;
	}

	return Math::GetFinalDepth(depth, nearPlane, farPlane);
}

float4 main(VS_OUTPUT input) : SV_Target0
{
	float2 sampleTexCoord = UnderwaterDepthOfFieldFlags.y != 0.0 ?
	                            FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord) :
	                            input.TexCoord;
	float4 color = SourceTex.SampleLevel(LinearSampler, sampleTexCoord, 0.0);
	float rawDepth = DepthTex.SampleLevel(PointSampler, sampleTexCoord, 0.0);
	float mask = 1.0;
	if (UnderwaterDepthOfFieldFlags.x != 0.0) {
		mask = MaskTex.SampleLevel(LinearSampler, sampleTexCoord, 0.0);
	}
	float fogFactor = FogColor.w * saturate((GetResolvedFinalDepth(rawDepth, mask) - FogRange.y) / (FogRange.x - FogRange.y)) * mask;

	color.rgb = lerp(color.rgb, FogColor.rgb, fogFactor);
	return color;
}
#endif
