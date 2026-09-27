/**
 * @file HDROutputCS.hlsl
 * @brief HDR: gamma decode, paper-white × (nits/203), BT.2020, PQ. SDR: passthrough + UI.
 */

#include "Common/Color.hlsli"
#include "Common/DisplayMapping.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/UIComposition.hlsli"

Texture2D<float4> SceneTex : register(t0);
Texture2D<float4> UITex : register(t1);
RWTexture2D<float4> HDROutput : register(u0);

cbuffer PerFrame : register(b0)
{
	float enableHDR : packoffset(c0.x);
	float paperWhite : packoffset(c0.y);
	float peakNits : packoffset(c0.z);
	float skipUIComposite : packoffset(c0.w);
	float uiBrightness : packoffset(c1.x);
	float isSceneLinear : packoffset(c1.y);
	float isMainOrLoadingMenu : packoffset(c1.z);
	float fgTweenMenuMidAlphaBoost : packoffset(c1.w);  ///< TweenMenu: soften AA band when compositing here (UIBrightnessCS skips while paused)
	float previewSDR : packoffset(c2.x);                ///< 1.0 = emit sRGB SDR (crop preview) instead of PQ HDR10
	float applyAutoHDR : packoffset(c2.y);              ///< 1.0 = Effects11 replaced ISHDR, so expand its SDR result into HDR
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint width, height;
	HDROutput.GetDimensions(width, height);
	if (dispatchID.x >= width || dispatchID.y >= height)
		return;

	float4 scene = SceneTex[dispatchID.xy];
	float4 ui = UITex[dispatchID.xy];

	bool hdrEnabled = enableHDR > 0.5;
	bool skipUI = skipUIComposite > 0.5;
	bool isMainLoading = isMainOrLoadingMenu > 0.5;
	bool postProcessOutput = SharedData::postProcessingSettings.DisableVanillaTonemapping != 0 && !isMainLoading;

	float3 finalColor;

	if (hdrEnabled) {
		bool sceneIsLinear = isSceneLinear > 0.5 || postProcessOutput;

		if (applyAutoHDR > 0.5) {
			float3 outputColor = sceneIsLinear ? scene.xyz : Color::GammaToLinearSafe(scene.xyz);
			outputColor = DisplayMapping::PumboAutoHDR(outputColor, SharedData::HDRData.z, SharedData::HDRData.y, 2.75, 1.0);
			scene.xyz = sceneIsLinear ? outputColor : Color::LinearToGammaSafe(outputColor);
		}

		float3 compositedColor = sceneIsLinear ? max(0.0, scene.rgb) : scene.rgb;
		if (!skipUI)
			compositedColor = UIComposition::CompositeSDR(compositedColor, ui, sceneIsLinear, postProcessOutput, isMainLoading ? 1.0 : uiBrightness);
		float3 compositedColorLinear = sceneIsLinear ? compositedColor : Color::GammaToLinearSafe(compositedColor);

		if (previewSDR > 0.5) {
			// Crop preview lives in the SDR menu buffer: emit sRGB instead of PQ.
			if (postProcessOutput)
				compositedColorLinear = Color::BT2020ToBT709(compositedColorLinear);
			finalColor = saturate(Color::LinearToSrgb(max(0.0, compositedColorLinear)));
		} else {
			if (!postProcessOutput)
				compositedColorLinear = Color::BT709ToBT2020(compositedColorLinear);
			finalColor = Color::pq::Encode(max(0.0, compositedColorLinear), paperWhite);

			finalColor = saturate(finalColor);
		}
	} else {
		float3 sceneGamma = scene.rgb;

		if (skipUI) {
			finalColor = sceneGamma;
		} else {
			finalColor = ui.rgb + sceneGamma * (1.0 - ui.a);
		}

		finalColor = saturate(finalColor);
	}

	HDROutput[dispatchID.xy] = float4(finalColor, 1.0);
}
