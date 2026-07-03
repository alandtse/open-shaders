// Stereo Reproject - Class A (view-independent) cross-eye transfer for SSGI diffuse
//
// AO and diffuse indirect lighting depend only on world geometry + incoming light, so
// the value at a world point is identical in both eyes. Rather than march both eyes
// and bilaterally blend (stereoSync.cs.hlsl), march eye 0 only and transfer its result
// into eye 1 by reprojection, falling back to eye 1's value (GI-absent, since its march
// was skipped) where eye 0 cannot see the point. Runs before the blur so the blur's
// cross-eye seam taps read valid eye-1 data. Same bindings as stereoSync.cs.hlsl.
//
// Based on: Nehab et al. 2007, "Accelerating Real-Time Shading with Reverse
// Reprojection Caching" (transfer view-independent shading, recompute on miss).

#include "Common/FrameBuffer.hlsli"
#include "Common/VR.hlsli"
#include "ScreenSpaceGI/common.hlsli"

#ifdef VR

Texture2D<float> srcDepth : register(t0);
Texture2D<float> srcAo : register(t1);
Texture2D<float4> srcIlY : register(t2);
Texture2D<float2> srcIlCoCg : register(t3);

RWTexture2D<float> outAo : register(u0);
RWTexture2D<float4> outIlY : register(u1);
RWTexture2D<float2> outIlCoCg : register(u2);

static const float kDepthAgreeThreshold = 0.05;  // NDC depth diff above which the reprojected eye-0 point is a different surface (disocclusion)

// Writes all output channels from the source buffers (passthrough / no-transfer path).
void Passthrough(uint2 dtid)
{
	outAo[dtid] = srcAo[dtid];
	outIlY[dtid] = srcIlY[dtid];
	outIlCoCg[dtid] = srcIlCoCg[dtid];
}

// Convert SSGI's linear view-space Z to raw NDC Z, matching stereoSync.cs.hlsl.
float LinearToRawDepth(float d)
{
	return (SharedData::CameraData.x - SharedData::CameraData.w / d) / SharedData::CameraData.z;
}

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	const float2 outFrameDim = OUT_FRAME_DIM;
	if (any(dtid >= uint2(outFrameDim)))
		return;

	const float2 frameScale = FrameDim * RcpTexDim;
	float2 uv = (dtid + 0.5) / outFrameDim;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	// Eye 0 is the reference: keep its natively marched GI unchanged.
	if (eyeIndex == 0) {
		Passthrough(dtid);
		return;
	}

	// SSGI working depth is linear view-space Z; below FP_Z is HMD mask / first-person hands.
	float depth = srcDepth.SampleLevel(samplerPointClamp, uv * frameScale, RES_MIP);
	if (depth < FP_Z) {
		Passthrough(dtid);
		return;
	}

	float rawDepth = LinearToRawDepth(depth);
	Stereo::StereoBilateralResult r = Stereo::ReprojectToOtherEye(uv, rawDepth, eyeIndex, outFrameDim);
	if (!r.valid) {
		Passthrough(dtid);  // off eye 0's frame: disocclusion, keep eye 1's value
		return;
	}

	float otherDepth = srcDepth.SampleLevel(samplerPointClamp, r.otherStereoUV * frameScale, RES_MIP);
	if (otherDepth < FP_Z) {
		Passthrough(dtid);
		return;
	}

	if (abs(LinearToRawDepth(otherDepth) - rawDepth) > kDepthAgreeThreshold) {
		Passthrough(dtid);  // eye 0 sees a different surface: disocclusion
		return;
	}

	// Surfaces agree: GI is view-independent, transfer eye 0's value exactly.
	outAo[dtid] = srcAo[r.otherPx];
	outIlY[dtid] = srcIlY[r.otherPx];
	outIlCoCg[dtid] = srcIlCoCg[r.otherPx];
}

#endif  // VR
