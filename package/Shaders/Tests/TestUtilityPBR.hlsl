#define CSHADER
#define TRUE_PBR
#include "/Shaders/Common/SharedData.hlsli"

namespace SharedData
{
	static LinearLightingSettings linearLightingSettings = (LinearLightingSettings)0;
	static CSUtilitySettings csUtilitySettings = (CSUtilitySettings)0;
}

#include "/Shaders/Common/Color.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags color, utility, lighting, pbr
[numthreads(1, 1, 1)] void TestUtilityPBRNormalization() {
	SharedData::csUtilitySettings.directionalLightColor = float2(0.5, -0.5);
	SharedData::csUtilitySettings.pointLightColor = float2(0.5, -0.5);
	SharedData::csUtilitySettings.spotlightColor = float2(1.0, 0.0);
	SharedData::csUtilitySettings.directionalLightMult = 1.0;
	SharedData::csUtilitySettings.pointLightMult = 1.0;
	SharedData::csUtilitySettings.spotlightMult = 1.0;
	const float3 input = float3(0.05, 0.1, 0.3);
	float3 expected = Color::AdjustUtilityColor(input, float2(0.5, -0.5)) * Math::PI;
	ASSERT(IsTrue, all(abs(Color::DirectionalLight(input) - expected) < 0.000001));
	ASSERT(IsTrue, all(abs(Color::PointLight(input, false, Color::PointLightFlagSpot) - expected) < 0.000001));
}
