#include "Common/Color.hlsli"
#include "Common/VR.hlsli"
#include "PostProcessing/fullscreen.hlsli"

static const uint GamutACEScg = 1;
static const uint GamutRec2020 = 2;

Texture2D<float4> texSrc : register(t0);
SamplerState linearSampler : register(s0);
cbuffer CopyConstants : register(b1)
{
	uint inputGamut;
	uint outputGamut;
	float gamma;
	uint resample;
};

float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	float4 color;
	if (resample) {
		uint2 dimensions;
		texSrc.GetDimensions(dimensions.x, dimensions.y);
		float2 uv = Stereo::ClampToEyeUV(input.TexCoord, Stereo::GetEyeIndexFromTexCoord(input.TexCoord), dimensions);
		color = texSrc.SampleLevel(linearSampler, uv, 0);
	} else {
		color = texSrc.Load(int3(input.Position.xy, 0));
	}
	if (inputGamut != outputGamut) {
		if (inputGamut == GamutACEScg)
			color.rgb = AP1TosRGB(color.rgb);
		else if (inputGamut == GamutRec2020)
			color.rgb = mul(XYZ_2_sRGB_MAT, mul(Rec2020_2_XYZ_MAT, color.rgb));
		if (outputGamut == GamutACEScg)
			color.rgb = sRGBToAP1(color.rgb);
		else if (outputGamut == GamutRec2020)
			color.rgb = mul(XYZ_2_Rec2020_MAT, mul(sRGB_2_XYZ_MAT, color.rgb));
	}
	if (gamma != 1.0)
		color.rgb = Color::SignedPow(color.rgb, gamma);
	return color;
}
