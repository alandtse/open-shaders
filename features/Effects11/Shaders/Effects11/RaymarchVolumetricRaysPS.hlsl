#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#define LinearSampler defaultSampler
SamplerState defaultSampler : register(s0);

#include "Common/ShadowSampling.hlsli"

#include "Effects11/VolumetricRaysCommon.hlsli"

struct VS_OUTPUT_POST
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

struct PS_OUTPUT
{
	float Scattering: SV_Target0;
	// Linear depth this texel raymarched with; the bilateral blur + upsample weight against it.
	float Depth: SV_Target1;
};

// Rays march no further than this (game units); a full-length ray reaches a visibility of 1
static const float MaxRayLength = 154117.64;
// Samples fade out with distance from the sun axis through the camera, reaching 0 at this radius
static const float RcpShadowCoverageRadiusSq = 1.0 / (262000.0 * 262000.0);

/** @brief Lit fraction of the view ray up to the pixel, scaled by how much of MaxRayLength it covers. */
float GetVolumetricRaysScattering(float3 positionMS, float noise, uint eyeIndex)
{
	float pixelDistance = length(positionMS);
	float3 rayDirection = positionMS / max(pixelDistance, 1e-4);
	float rayLength = min(pixelDistance, MaxRayLength);
	float3 sunDirection = SharedData::SunDirection.xyz;

	const uint sampleCount = SharedData::enbSettings.VolumetricRaysSampleCount;
	const float rcpSampleCount = 1.0 / float(sampleCount);

	float visibility = 0.0;

	[loop] for (uint i = 0; i < sampleCount; i++)
	{
		float3 samplePos = rayDirection * ((float(i) + noise) * rcpSampleCount * rayLength);

		float shadow = ShadowSampling::GetWorldShadow(samplePos, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

		float alongSun = dot(samplePos, sunDirection);
		float offsetSq = max(dot(samplePos, samplePos) - alongSun * alongSun, 0.0);
		visibility += shadow * saturate(1.0 - offsetSq * RcpShadowCoverageRadiusSq);
	}

	return saturate(visibility * rcpSampleCount * rayLength / MaxRayLength);
}

PS_OUTPUT main(VS_OUTPUT_POST input)
{
	Stereo::EyeUV eye = Stereo::UnpackEyeUV(input.txcoord0);
	float2 uv = eye.uv;

	float depth = SharedData::GetDepth(uv, eye.index);
	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse[eye.index], positionCS);
	positionMS.xyz /= positionMS.w;

	float noise = Random::InterleavedGradientNoise(Stereo::EyeStableNoiseCoord(input.pos.xy, ScreenSize), SharedData::FrameCount);
	PS_OUTPUT output;
	output.Scattering = GetVolumetricRaysScattering(positionMS.xyz, noise, eye.index);
	output.Depth = SharedData::GetScreenDepth(depth);
	return output;
}
