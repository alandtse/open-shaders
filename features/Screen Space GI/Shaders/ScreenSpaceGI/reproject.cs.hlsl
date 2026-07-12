// Stereo Reproject - view-independent cross-eye transfer for SSGI diffuse
//
// AO and diffuse indirect GI are view-independent, so eye 1 can take eye 0's value at
// the same world point. On a disocclusion miss, gi.cs marched eye 1 natively (gated by
// the same GIReprojectsCleanly test), so the fallback is a real value, not a hole.
// Must run before the blur, whose seam taps read across eyes.
//
// Based on: Nehab et al. 2007, "Accelerating Real-Time Shading with Reverse
// Reprojection Caching" (transfer view-independent shading, recompute on miss).

#include "Common/FrameBuffer.hlsli"
#include "Common/VR.hlsli"
#include "ScreenSpaceGI/StereoReproject.hlsli"
#include "ScreenSpaceGI/common.hlsli"

#ifdef VR

Texture2D<float> srcDepth : register(t0);
Texture2D<float> srcAo : register(t1);
Texture2D<float4> srcIlY : register(t2);
Texture2D<float2> srcIlCoCg : register(t3);

RWTexture2D<float> outAo : register(u0);
RWTexture2D<float4> outIlY : register(u1);
RWTexture2D<float2> outIlCoCg : register(u2);

// Writes all output channels from the source buffers (passthrough / no-transfer path).
void Passthrough(uint2 dtid)
{
	outAo[dtid] = srcAo[dtid];
	outIlY[dtid] = srcIlY[dtid];
	outIlCoCg[dtid] = srcIlCoCg[dtid];
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

	float depth = srcDepth.SampleLevel(samplerPointClamp, uv * frameScale, RES_MIP);
	int2 otherPx;
	if (GIReprojectsCleanly(uv, depth, eyeIndex, srcDepth, frameScale, otherPx)) {
		// Surfaces agree: GI is view-independent, transfer eye 0's value exactly.
		outAo[dtid] = srcAo[otherPx];
		outIlY[dtid] = srcIlY[otherPx];
		outIlCoCg[dtid] = srcIlCoCg[otherPx];
	} else {
		// Disocclusion: gi.cs marched this pixel natively (same test), keep it.
		Passthrough(dtid);
	}
}

#endif  // VR
