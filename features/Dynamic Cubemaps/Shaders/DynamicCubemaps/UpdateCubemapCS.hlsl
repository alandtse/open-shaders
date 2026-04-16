#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

RWTexture2DArray<float4> DynamicCubemap : register(u0);
RWTexture2DArray<float4> DynamicCubemapRaw : register(u1);
RWTexture2DArray<float4> DynamicCubemapPosition : register(u2);

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> ColorTexture : register(t1);

SamplerState LinearSampler : register(s0);

// Calculate normalized sampling direction vector based on current fragment coordinates.
// This is essentially "inverse-sampling": we reconstruct what the sampling vector would be if we wanted it to "hit"
// this particular fragment in a cubemap.
float3 GetSamplingVector(uint3 ThreadID, in RWTexture2DArray<float4> OutputTexture)
{
	float width = 0.0f;
	float height = 0.0f;
	float depth = 0.0f;
	OutputTexture.GetDimensions(width, height, depth);

	float2 st = ThreadID.xy / float2(width, height);
	float2 uv = 2.0 * float2(st.x, 1.0 - st.y) - 1.0;

	// Select vector based on cubemap face index.
	float3 result = float3(0.0f, 0.0f, 0.0f);
	switch (ThreadID.z) {
	case 0:
		result = float3(1.0, uv.y, -uv.x);
		break;
	case 1:
		result = float3(-1.0, uv.y, uv.x);
		break;
	case 2:
		result = float3(uv.x, 1.0, -uv.y);
		break;
	case 3:
		result = float3(uv.x, -1.0, uv.y);
		break;
	case 4:
		result = float3(uv.x, uv.y, 1.0);
		break;
	case 5:
		result = float3(-uv.x, uv.y, -1.0);
		break;
	}
	return normalize(result);
}

float GetFrameEdgeMask(float2 uvMono)
{
	float2 edgeDistance = min(uvMono, 1.0 - uvMono);
	float minEdgeDistance = min(edgeDistance.x, edgeDistance.y);
	const float edgeFadeStart = 0.015;
	const float edgeFadeEnd = 0.14;
	return smoothstep(edgeFadeStart, edgeFadeEnd, minEdgeDistance);
}

cbuffer UpdateData : register(b0)
{
	float3 CameraPreviousPosAdjust2;
	uint CaptureFlags;
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID
								: SV_DispatchThreadID) {
	float3 captureDirection = -GetSamplingVector(ThreadID, DynamicCubemap);
	float3 viewDirection = FrameBuffer::WorldToView(captureDirection, false);
	float2 uv = FrameBuffer::ViewToUV(viewDirection, false);
	const bool disableForwardCaptureGate = ((CaptureFlags & 0x1u) != 0);
	const bool suppressSkyAndFrameEdgeCapture = ((CaptureFlags & 0x2u) != 0);

	bool shouldTryCapture = true;
#if !defined(VR)
	shouldTryCapture = !FrameBuffer::IsOutsideFrame(uv);
#endif

	if (shouldTryCapture) {  // Check that the view direction exists in screenspace and that it is in front of the camera
		float3 color = 0.0;
		float3 position = 0.0;
		float weight = 0.0;
		// Wider/softer capture gate to reduce player-attached looking cutoffs:
		// - extend side/slightly-behind coverage (forwardGateEnd)
		// - avoid strong radial depth weighting (prevents concentric ring look)
		const float forwardGateStart = -0.12;
		const float forwardGateEnd = 0.55;
		const float nearDepthValidityStart = 0.75;
		const float nearDepthValidityEnd = 2.75;
		const float nearDepthWeightFloor = 0.88;

#if !defined(VR)
		uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
#endif

#if defined(VR)
		[unroll] for (uint eyeIndex = 0; eyeIndex < 2; eyeIndex++) {
			float3 eyeViewDirection = FrameBuffer::WorldToView(captureDirection, false, eyeIndex);
			float2 eyeUVMono = FrameBuffer::ViewToUV(eyeViewDirection, false, eyeIndex);

			if (FrameBuffer::IsOutsideFrame(eyeUVMono))
				continue;

			// Soft forward visibility gate:
			// - full weight in front
			// - gradual falloff toward side/slightly-behind directions
			float forwardMask = 1.0 - smoothstep(forwardGateStart, forwardGateEnd, eyeViewDirection.z);
			forwardMask = smoothstep(0.0, 1.0, sqrt(saturate(forwardMask)));
			if (disableForwardCaptureGate)
				forwardMask = 1.0;
			if (forwardMask <= 1e-3)
				continue;
			float frameEdgeMask = suppressSkyAndFrameEdgeCapture ? GetFrameEdgeMask(eyeUVMono) : 1.0;
			if (frameEdgeMask <= 1e-3)
				continue;

			float2 eyeUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(eyeUVMono, 0);
			eyeUV = Stereo::ConvertToStereoUV(eyeUV, eyeIndex);
			float depth = DepthTexture.SampleLevel(LinearSampler, eyeUV, 0);
			float linearDepth = SharedData::GetScreenDepth(depth);
			// Near-depth validity:
			// - reject extremely close samples (camera-near instability)
			// - keep remaining depth weighting subtle to avoid concentric rings.
			float nearDepthValidity = smoothstep(nearDepthValidityStart, nearDepthValidityEnd, linearDepth);
			if (nearDepthValidity <= 1e-3)
				continue;
			float nearDepthWeight = lerp(nearDepthWeightFloor, 1.0, nearDepthValidity);

#	if defined(REFLECTIONS)
			float nonSkyMask = suppressSkyAndFrameEdgeCapture ? (1.0 - smoothstep(0.9988, 0.99997, depth)) : 1.0;
			float sampleWeight = forwardMask * nearDepthWeight * frameEdgeMask * nonSkyMask;
#	else
			// Soft sky rejection to reduce blocky transitions at geometry/sky boundaries.
			float nonSkyMask = 1.0 - smoothstep(0.9992, 0.99995, depth);
			float sampleWeight = forwardMask * nearDepthWeight * nonSkyMask * frameEdgeMask;
#	endif
			if (sampleWeight > 1e-3) {
				half4 positionCS = half4(2 * half2(eyeUVMono.x, -eyeUVMono.y + 1) - 1, depth, 1);
				positionCS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionCS);
				positionCS.xyz = positionCS.xyz / positionCS.w;

				position += positionCS.xyz * sampleWeight;
				color += ColorTexture.SampleLevel(LinearSampler, eyeUV, 0).rgb * sampleWeight;
				weight += sampleWeight;
			}
		}
#else
		float2 eyeUV = Stereo::ConvertToStereoUV(uv, 0);
		float depth = DepthTexture.SampleLevel(LinearSampler, eyeUV, 0);
		float linearDepth = SharedData::GetScreenDepth(depth);
		float forwardMask = 1.0 - smoothstep(forwardGateStart, forwardGateEnd, viewDirection.z);
		forwardMask = smoothstep(0.0, 1.0, sqrt(saturate(forwardMask)));
		if (disableForwardCaptureGate)
			forwardMask = 1.0;
		float nearDepthValidity = smoothstep(nearDepthValidityStart, nearDepthValidityEnd, linearDepth);
		if (nearDepthValidity > 1e-3) {
			float nearDepthWeight = lerp(nearDepthWeightFloor, 1.0, nearDepthValidity);
			float frameEdgeMask = suppressSkyAndFrameEdgeCapture ? GetFrameEdgeMask(uv) : 1.0;

#	if defined(REFLECTIONS)
			float nonSkyMask = suppressSkyAndFrameEdgeCapture ? (1.0 - smoothstep(0.9988, 0.99997, depth)) : 1.0;
			float sampleWeight = forwardMask * nearDepthWeight * frameEdgeMask * nonSkyMask;
#	else
			float nonSkyMask = 1.0 - smoothstep(0.9992, 0.99995, depth);
			float sampleWeight = forwardMask * nearDepthWeight * nonSkyMask * frameEdgeMask;
#	endif
			if (sampleWeight > 1e-3) {
				half4 positionCS = half4(2 * half2(eyeUV.x, -eyeUV.y + 1) - 1, depth, 1);
				positionCS = mul(FrameBuffer::CameraViewProjInverse[0], positionCS);
				positionCS.xyz = positionCS.xyz / positionCS.w;

				position += positionCS.xyz * sampleWeight;
				color += ColorTexture.SampleLevel(LinearSampler, eyeUV, 0).rgb * sampleWeight;
				weight += sampleWeight;
			}
		}
#endif

		if (weight > 0.0) {
			position /= weight;
			color /= weight;

			float4 positionFinal = float4(position.xyz * 0.001, length(position) < (4096.0 * 2.5));
			float4 colorFinal = float4(Color::IrradianceToLinear(color), 1.0);

			float lerpFactor = 0.5;

			DynamicCubemapPosition[ThreadID] = lerp(DynamicCubemapPosition[ThreadID], positionFinal, lerpFactor);
			DynamicCubemapRaw[ThreadID] = max(0, lerp(DynamicCubemapRaw[ThreadID], colorFinal, lerpFactor));

			colorFinal *= sqrt(saturate(0.5 * length(position.xyz)));

			DynamicCubemap[ThreadID] = max(0, lerp(DynamicCubemap[ThreadID], colorFinal, lerpFactor));

			return;
		}
	}

	float4 position = DynamicCubemapPosition[ThreadID];
	float3 cameraPosAdjustCenter = FrameBuffer::CameraPosAdjust[0].xyz;
#if defined(VR)
	cameraPosAdjustCenter = 0.5 * (FrameBuffer::CameraPosAdjust[0].xyz + FrameBuffer::CameraPosAdjust[1].xyz);
#endif
	position.xyz = (position.xyz + (CameraPreviousPosAdjust2.xyz * 0.001)) - (cameraPosAdjustCenter * 0.001);  // Remove adjustment, add new adjustment
	DynamicCubemapPosition[ThreadID] = position;

	float4 color = DynamicCubemapRaw[ThreadID];
	// Temporal history decay for texels not refreshed this frame.
	// Avoid radial distance weighting here: it produces concentric player-centered rings in motion.
	float historyFade = 0.94;
#if defined(FAKEREFLECTIONS)
	historyFade = 0.97;
#endif
	if (suppressSkyAndFrameEdgeCapture) {
#if defined(REFLECTIONS)
		historyFade = min(historyFade, 0.86);
#else
		historyFade = min(historyFade, 0.90);
#endif
	}
	color *= historyFade;

	DynamicCubemap[ThreadID] = max(0, color);
}
