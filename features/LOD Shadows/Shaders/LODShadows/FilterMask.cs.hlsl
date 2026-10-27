#include "Common/SharedData.hlsli"
#include "LODShadows/LODShadows.hlsli"

cbuffer MaskData : register(b0)
{
	uint2 MaskDimensions;
	uint2 Padding;
};

Texture2D<float> SceneDepth : register(t0);
Texture2D<float4> InputMask : register(t1);
RWTexture2D<float4> OutputMask : register(u0);
SamplerState MaskSampler : register(s0);

static const int2 CrossOffsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
static const float CrossCenterWeight = 4.0;
static const float MinProjectionW = 1e-8;
static const uint MaskDownsample = 2;

float3 GetPosition(uint2 pixel, uint eyeIndex)
{
	uint2 depthDimensions;
	SceneDepth.GetDimensions(depthDimensions.x, depthDimensions.y);
	uint2 activeDimensions = uint2(float2(depthDimensions) * FrameBuffer::DynamicResolutionParams1.xy);
#if defined(VR)
	pixel.x -= eyeIndex * (MaskDimensions.x / 2);
	activeDimensions.x /= 2;
#endif
	uint2 depthPixel = min(pixel * MaskDownsample + MaskDownsample / 2, activeDimensions - 1);
	float2 monoUV = (depthPixel + 0.5) / activeDimensions;
#if defined(VR)
	depthPixel.x += eyeIndex * activeDimensions.x;
#endif
	float depth = SceneDepth.Load(int3(depthPixel, 0));
	if (!FrameBuffer::IsReverseProjection(eyeIndex))
		depth = FrameBuffer::ToStandardDepth(depth);
	float4 position = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], float4(monoUV * float2(2, -2) + float2(-1, 1), depth, 1));
	return position.xyz / max(position.w, MinProjectionW);
}

[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= MaskDimensions))
		return;
	float2 uv = (id + 0.5) / MaskDimensions;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	int2 lower = 0;
	int2 upper = int2(MaskDimensions) - 1;
#if defined(VR)
	lower.x = eyeIndex * int(MaskDimensions.x / 2);
	upper.x = lower.x + int(MaskDimensions.x / 2) - 1;
#endif
#if defined(FILTER)
	float4 center = InputMask.Load(int3(id, 0));
	float3 result = float3(center.xy, 1) * CrossCenterWeight;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		float4 sample = InputMask.Load(int3(clamp(int2(id) + CrossOffsets[i], lower, upper), 0));
		float weight = LODShadows::MaskDepthWeight(center.z, sample.z);
		result += float3(sample.xy, 1) * weight;
	}
	OutputMask[id] = float4(result.xy / result.z, center.zw);
#else
	float3 position = GetPosition(id, eyeIndex);
	float3 dx = GetPosition(clamp(int2(id) + int2(1, 0), lower, upper), eyeIndex) - position;
	float3 dy = GetPosition(clamp(int2(id) + int2(0, 1), lower, upper), eyeIndex) - position;
	float depth = log2(max(LODShadows::MinMaskDepth, mul(FrameBuffer::CameraView[eyeIndex], float4(position, 1)).z));
	float coverage;
	float visibility = LODShadows::FilterShadow(position + FrameBuffer::CameraPosAdjust[eyeIndex].xyz,
		dx, dy, MaskSampler, coverage, false, LODShadows::GetObjectShadowWeight(position, eyeIndex));
	OutputMask[id] = float4(visibility, coverage, depth, 1);
#endif
}
