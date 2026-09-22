#include "Common/SharedData.hlsli"
#include "Common/UIComposition.hlsli"
#include "Menu/BackgroundBlur.hlsli"

Texture2D<float4> UITexture : register(t1);

static const float kMainLoadingSceneThreshold = 0.9;

float4 PS_Copy(VS_OUTPUT input) : SV_TARGET
{
	return InputTexture.SampleLevel(LinearSampler, input.TexCoord, 0);
}

float4 PS_Downsample(VS_OUTPUT input) : SV_TARGET
{
	float2 offset = BlurTextureSize.zw * BackgroundBlur::kDownsampleOffset;
	return (InputTexture.SampleLevel(LinearSampler, input.TexCoord + offset, 0) +
			   InputTexture.SampleLevel(LinearSampler, input.TexCoord - offset, 0) +
			   InputTexture.SampleLevel(LinearSampler, input.TexCoord + float2(offset.x, -offset.y), 0) +
			   InputTexture.SampleLevel(LinearSampler, input.TexCoord + float2(-offset.x, offset.y), 0)) *
	       0.25f;
}

float4 PS_HDRDownsample(VS_OUTPUT input) : SV_TARGET
{
	float4 scene = PS_Downsample(input);
	float2 offset = BlurTextureSize.zw * BackgroundBlur::kDownsampleOffset;
	float4 ui = (UITexture.SampleLevel(LinearSampler, input.TexCoord + offset, 0) +
					UITexture.SampleLevel(LinearSampler, input.TexCoord - offset, 0) +
					UITexture.SampleLevel(LinearSampler, input.TexCoord + float2(offset.x, -offset.y), 0) +
					UITexture.SampleLevel(LinearSampler, input.TexCoord + float2(-offset.x, offset.y), 0)) *
	            0.25f;
	bool isMainLoading = SharedData::HDRData.w > kMainLoadingSceneThreshold;
	bool postProcessOutput = SharedData::postProcessingSettings.DisableVanillaTonemapping != 0 && !isMainLoading;
	bool sceneIsLinear = SharedData::linearLightingSettings.enableLinearLighting || postProcessOutput;
	scene.rgb = UIComposition::CompositeSDR(scene.rgb, ui, sceneIsLinear, postProcessOutput, isMainLoading ? 1.0 : UIParams.x);
	return scene;
}

float4 PS_Main(VS_OUTPUT input) : SV_TARGET
{
	return BackgroundBlur::SampleGaussian(input.TexCoord, float2(BlurTextureSize.z, 0.0f));
}
