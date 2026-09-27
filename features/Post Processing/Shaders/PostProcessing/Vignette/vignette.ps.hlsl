#include "Common/VR.hlsli"
#include "PostProcessing/fullscreen.hlsli"

Texture2D<float4> TexColor : register(t0);

cbuffer VignetteCB : register(b1)
{
	float4 Params0;  // focal, anamorphism (included in aspect ratio), power, aspect ratio
	float4 RcpDynRes;
};

float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	uint2 tid = uint2(input.Position.xy);
	float3 color = TexColor[tid].rgb;

	float2 uv = (tid + .5) * RcpDynRes.xy;

	// Vignette falloff is centered on the screen; in VR the packed stereo buffer's
	// midpoint is the seam between eyes, not either eye's center, so recenter per-eye.
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	float2 eyeUV = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float cos_view = length((eyeUV - .5) * float2(1, Params0.w));
	cos_view = Params0.x * rsqrt(cos_view * cos_view + Params0.x * Params0.x);
	float vignette = pow(max(cos_view, 0.0), Params0.z);

	color *= vignette;

	return float4(color, 1);
}
