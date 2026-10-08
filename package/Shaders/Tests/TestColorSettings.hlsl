#define CSHADER
#define EFFECTS11
#include "/Shaders/Common/SharedData.hlsli"

// Namespace statics let the GPU tests supply settings without binding the game's constant buffers.
namespace SharedData
{
	static LinearLightingSettings linearLightingSettings = (LinearLightingSettings)0;
	static CSUtilitySettings csUtilitySettings = (CSUtilitySettings)0;
	static ENBSettings enbSettings = (ENBSettings)0;
}

#include "/Shaders/Common/Color.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags color, gamma, lighting
[numthreads(1, 1, 1)] void TestAuthoredGammaOffsetContinuity() {
	SharedData::linearLightingSettings.enableLinearLighting = true;
	SharedData::linearLightingSettings.conversionSaturation = 1.0;
	const float samples[6] = { 0.001, 0.02, 0.04045, 0.18, 0.5, 0.99 };
	const float smallOffset = 0.0001;
	for (int i = 0; i < 6; i++) {
		float3 encoded = samples[i].xxx;
		float3 reference = Color::DecodeSRGB(encoded);
		float3 unchanged = Color::AdjustedAuthoredColor(encoded, 0.0);
		float3 brighter = Color::AdjustedAuthoredColor(encoded, -smallOffset);
		float3 darker = Color::AdjustedAuthoredColor(encoded, smallOffset);
		ASSERT(IsTrue, all(abs(unchanged - reference) < 0.0000001));
		ASSERT(IsTrue, all(brighter >= reference) && all(darker <= reference));
		ASSERT(IsTrue, all(abs(brighter / reference - 1.0) < 0.002));
		ASSERT(IsTrue, all(abs(darker / reference - 1.0) < 0.002));
	}
	float3 signedColor = float3(-0.02, 0.0, 4.0);
	float3 signedReference = Color::DecodeSRGB(signedColor);
	float3 adjusted = Color::AdjustedAuthoredColor(signedColor, smallOffset);
	ASSERT(IsTrue, all(isfinite(adjusted)));
	ASSERT(IsTrue, all(sign(adjusted) == sign(signedColor)));
	ASSERT(IsTrue, all(abs(adjusted - signedReference) < 0.001 * max(abs(signedReference), 0.00001)));
}

	/// @tags color, gamma, lighting
	[numthreads(1, 1, 1)] void TestVolumetricGammaConsistency()
{
	SharedData::linearLightingSettings.conversionSaturation = 1.0;
	SharedData::csUtilitySettings.vlIntensity = 1.5;
	const float offsets[3] = { -0.01, 0.0, 0.01 };
	for (uint enabled = 0; enabled < 2; enabled++) {
		SharedData::linearLightingSettings.enableLinearLighting = enabled;
		for (int i = 0; i < 3; i++) {
			SharedData::csUtilitySettings.vlGammaOffset = offsets[i];
			float3 reference = Color::AdjustedAuthoredColor(0.02.xxx, offsets[i]);
			float intensity = Color::VolumetricLighting(0.02);
			ASSERT(IsTrue, abs(intensity - reference.x * 1.5) < 0.0000001);
			if (!enabled)
				ASSERT(IsTrue, abs(reference.x - pow(0.02, 1.0 + offsets[i])) < 0.0000001);
		}
	}
}

/// @tags color, lighting, effects11
[numthreads(1, 1, 1)] void TestProjectedDiffuseEffects11() {
	SharedData::enbSettings.Enable = true;
	const float powers[3] = { 1.0, 1.5, 2.2 };
	const float3 textureColor = float3(0.2, 0.5, 0.8);
	const float3 tint = float3(0.5, 0.7, 1.0);
	for (int i = 0; i < 3; i++) {
		SharedData::enbSettings.ColorPow = powers[i];
		float3 expected = pow(textureColor * tint, powers[i]);
		float3 projected = Color::ProjectedDiffuse(textureColor, tint, 1.0.xxx);
		ASSERT(IsTrue, all(abs(projected - expected) < 0.000001));
	}
	SharedData::enbSettings.Enable = false;
	ASSERT(IsTrue, all(abs(Color::ProjectedDiffuse(textureColor, tint, 1.0.xxx) - textureColor * tint) < 0.000001));
}

	/// @tags color, lighting, particles
	[numthreads(1, 1, 1)] void TestParticleAuthoredTint()
{
	SharedData::linearLightingSettings.enableLinearLighting = true;
	SharedData::linearLightingSettings.conversionSaturation = 1.0;
	float3 neutral = Color::TextureToWorking(0.5.xxx, 0.5.xxx);
	ASSERT(IsTrue, all(abs(neutral - 0.0614671765.xxx) < 0.000001));
	const float3 textureColor = float3(0.8, 0.1, 0.4);
	const float3 tint = float3(0.2, 0.7, 0.5);
	float3 untinted = Color::TextureToWorking(textureColor);
	ASSERT(IsTrue, all(abs(untinted - pow(textureColor, Color::LegacyTextureGamma)) < 0.000001));
	float3 linearResult = Color::TextureToWorking(textureColor, tint);
	SharedData::linearLightingSettings.enableACEScg = true;
	float3 acescgResult = Color::TextureToWorking(textureColor, tint);
	ASSERT(IsTrue, all(abs(AP1TosRGB(acescgResult) - linearResult) < 0.000001));
	SharedData::linearLightingSettings.enableACEScg = false;
	SharedData::linearLightingSettings.conversionSaturation = 0.92;
	float3 compensated = Color::TextureToWorking(textureColor, tint);
	ASSERT(IsTrue, all(abs(compensated - float3(0.0364184598, 0.0122965983, 0.0426716938)) < 0.000001));
	SharedData::linearLightingSettings.enableLinearLighting = false;
	SharedData::enbSettings.Enable = true;
	SharedData::enbSettings.ColorPow = 2.0;
	float3 legacy = Color::EnbColorPow(Color::TextureToWorking(0.5.xxx, 0.5.xxx));
	ASSERT(IsTrue, all(abs(legacy - 0.0625.xxx) < 0.000001));
}

/// @tags color, gamma, effects
[numthreads(1, 1, 1)] void TestEffectSceneTransfer() {
	SharedData::linearLightingSettings.conversionSaturation = 0.92;
	const float3 samples[4] = { 0.0.xxx, 0.18.xxx, float3(0.8, 0.3, 0.02), float3(-0.02, 0.0, 4.0) };
	for (uint enabled = 0; enabled < 2; enabled++) {
		SharedData::linearLightingSettings.enableLinearLighting = enabled;
		for (uint acescg = 0; acescg < 2; acescg++) {
			SharedData::linearLightingSettings.enableACEScg = acescg;
			for (uint i = 0; i < 4; i++) {
				float3 workingColor = enabled ? Color::GamutTransform(samples[i]) : samples[i];
				float3 encoded = Color::EffectLightToGamma(workingColor);
				ASSERT(IsTrue, all(abs(Color::EffectLight(samples[i], true) - workingColor) < 0.00001));
				ASSERT(IsTrue, all(abs(Color::EffectLight(encoded) - workingColor) < 0.00001));
				if (enabled) {
					ASSERT(IsTrue, all(abs(Color::SceneGammaToLinear(encoded) - workingColor) < 0.00001));
				} else {
					ASSERT(IsTrue, all(encoded == samples[i]));
				}
			}
		}
	}
}

	/// @tags color, gamma, effects
	[numthreads(1, 1, 1)] void TestEffectLightingGain()
{
	SharedData::linearLightingSettings.enableLinearLighting = true;
	const float gains[4] = { 0.0, 0.25, 1.0, 4.0 };
	for (uint acescg = 0; acescg < 2; acescg++) {
		SharedData::linearLightingSettings.enableACEScg = acescg;
		float3 radiance = Color::GamutTransform(float3(0.18, 0.5, 2.0));
		for (uint i = 0; i < 4; i++) {
			SharedData::csUtilitySettings.effectLightingMult = gains[i];
			float3 encoded = Color::EffectLightToGamma(radiance) * Color::EffectLightingMultiplier();
			ASSERT(IsTrue, all(abs(Color::SceneGammaToLinear(encoded) - radiance * gains[i]) < 0.00001));
		}
	}
	SharedData::linearLightingSettings.enableLinearLighting = false;
	for (uint i = 0; i < 4; i++) {
		SharedData::csUtilitySettings.effectLightingMult = gains[i];
		ASSERT(AreEqual, Color::EffectLightingMultiplier(), gains[i]);
	}
}

/// @tags color, colorspace, lighting
[numthreads(1, 1, 1)] void TestProjectedTintGamut() {
	SharedData::linearLightingSettings.enableLinearLighting = true;
	SharedData::linearLightingSettings.diffuseGamma = 2.2;
	SharedData::linearLightingSettings.diffuseCurve = 3.59479342;
	SharedData::linearLightingSettings.diffuseWhiteReflectance = 1.0;
	const float3 textureColor = float3(0.8, 0.3, 0.2);
	const float3 tints[2] = { 1.0.xxx, float3(0.2, 0.7, 0.9) };
	const float saturations[3] = { 0.0, 0.92, 1.0 };
	for (uint i = 0; i < 3; i++) {
		SharedData::linearLightingSettings.conversionSaturation = saturations[i];
		for (uint j = 0; j < 2; j++) {
			SharedData::linearLightingSettings.enableACEScg = false;
			float3 linearSrgb = Color::ProjectedDiffuse(textureColor, tints[j], 1.0.xxx);
			SharedData::linearLightingSettings.enableACEScg = true;
			float3 acescg = Color::ProjectedDiffuse(textureColor, tints[j], 1.0.xxx);
			ASSERT(IsTrue, all(abs(AP1TosRGB(acescg) - linearSrgb) < 0.00001));
		}
	}
}
