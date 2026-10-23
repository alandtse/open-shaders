#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

Texture2D<float> BlurredShadowTexture : register(t0);
Texture2D<float> RaymarchDepthTexture : register(t1);

#include "Effects11/VolumetricRaysCommon.hlsli"

static const float UpsampleDepthBias = 0.01;

struct VS_OUTPUT_POST
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

// Depth weighting keeps the half-resolution scattering from bleeding across depth edges.
float UpsampleScattering(float2 fullResPixel, float fullResDepth, uint eyeIndex)
{
	float2 halfPixel = fullResPixel * 0.5 - 0.5;
	int2 basePixel = int2(floor(halfPixel));
	float2 fraction = halfPixel - basePixel;

	const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
	float4 bilinearWeights = float4(
		(1.0 - fraction.x) * (1.0 - fraction.y),
		fraction.x * (1.0 - fraction.y),
		(1.0 - fraction.x) * fraction.y,
		fraction.x * fraction.y);

	float referenceDepth = SharedData::GetScreenDepth(fullResDepth);

	float weightedSum = 0.0;
	float weightSum = 0.0;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		int2 tap = Stereo::ClampToEyeBounds(basePixel + offsets[i], eyeIndex, ScreenSize);
		float tapDepth = RaymarchDepthTexture[tap];
		float relativeDelta = abs(referenceDepth - tapDepth) / max(referenceDepth, VolumetricRays::DepthEpsilon);
		float weight = bilinearWeights[i] * rcp(UpsampleDepthBias + relativeDelta);
		weightedSum += weight * BlurredShadowTexture[tap];
		weightSum += weight;
	}
	return weightedSum / weightSum;
}

/** @brief Shapes the ray visibility: Density above 1 thins the rays, below 1 thickens them. */
float ApplyDensity(float visibility)
{
	float density = SharedData::enbSettings.VolumetricRaysDensity;
	if (density >= 1.0)
		return pow(visibility, density);
	return 1.0 - pow(1.0 - visibility, rcp(max(density, 0.001)));
}

float4 main(VS_OUTPUT_POST input) : SV_Target0
{
	Stereo::EyeUV eye = Stereo::UnpackEyeUV(input.txcoord0);
	float depth = SharedData::GetDepth(eye.uv, eye.index);
	float rays = ApplyDensity(saturate(UpsampleScattering(input.pos.xy, depth, eye.index)));

	// SunColor is already premultiplied by the sun's visibility
	float3 sunColor = max(SharedData::SunColor.xyz, 0.0);
	float sunPeak = max(max(max(sunColor.x, sunColor.y), sunColor.z), 1e-5);
	float3 skyColor = max(SharedData::enbSettings.VolumetricRaysSkyColor, 0.0) * (sunColor + 1e-5) / sunPeak;
	float skyAmount = SharedData::enbSettings.VolumetricRaysSkyColorAmount * saturate(dot(sunColor, 1.0) * 3.0);

	float3 lightColor = Color::Sky(sunColor) + Color::Sky(skyColor) * skyAmount;
	return float4(rays * lightColor * SharedData::enbSettings.VolumetricRaysIntensity, 0);
}
