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

static const uint SampleCount = 16;
static const float RcpSampleCount = 1.0 / float(SampleCount);

float GetVolumetricRaysScattering(float3 positionMS, float noise, uint eyeIndex)
{
	float negExtTimesRayLen = -SharedData::enbSettings.VolumetricRaysExtinction * length(positionMS);
	float scattering = 0.0;
	float transmittance = 1.0;

	[unroll] for (uint i = 0; i < SampleCount; i++)
	{
		float t0 = float(i) * RcpSampleCount;
		float t1 = float(i + 1) * RcpSampleCount;
		t0 *= t0;
		t1 *= t1;
		float3 samplePos = positionMS * lerp(t0, t1, noise);

		float shadow = ShadowSampling::GetWorldShadow(samplePos, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

		shadow *= shadow;
		float stepTransmittance = exp(negExtTimesRayLen * (t1 - t0));
		scattering += shadow * (1.0 - stepTransmittance) * transmittance;
		transmittance *= stepTransmittance;
	}

	return scattering;
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
