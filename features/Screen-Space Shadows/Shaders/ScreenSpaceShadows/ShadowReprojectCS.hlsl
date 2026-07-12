// VR: transfer eye 0's view-independent shadow into eye 1 by reprojection,
// falling back to eye 1's own value on disocclusion.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#ifdef VR

#	if defined(TERRAIN_BLENDING)
Texture2D<float> SrcDepthTexture : register(t0);
#	else
Texture2D<unorm float> SrcDepthTexture : register(t0);
#	endif
Texture2D<unorm float> SrcShadowTexture : register(t1);

RWTexture2D<unorm float> OutShadowTexture : register(u0);

cbuffer StereoSyncCB : register(b1)
{
	float2 FrameDim;
	float2 RcpFrameDim;
};

static const float kDepthAgreeThreshold = 0.05;  // NDC diff above which eye 0 sees a different surface

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	if (any(dtid >= uint2(FrameDim)))
		return;

	float2 uv = (dtid + 0.5) * RcpFrameDim;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	// Eye 0 is the reference; pass it through unchanged.
	if (eyeIndex == 0) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	float depth = SrcDepthTexture[dtid];
	// Sky (near-zero) and HMD-mask (>=1) pixels aren't geometry; pass through, don't reproject.
	if (depth < EPSILON_DEPTH_SKY || depth >= 1.0) {
		OutShadowTexture[dtid] = SrcShadowTexture[dtid];
		return;
	}

	// Fall back to eye 1's own value (unshadowed clear or native march) on disocclusion.
	float result = SrcShadowTexture[dtid];

	Stereo::StereoBilateralResult r = Stereo::ReprojectToOtherEye(uv, depth, eyeIndex, FrameDim);
	if (r.valid) {
		float otherDepth = SrcDepthTexture[r.otherPx];
		bool otherIsGeometry = otherDepth >= EPSILON_DEPTH_SKY && otherDepth < 1.0;
		if (otherIsGeometry && Stereo::IsReprojectionExact(r, depth, otherDepth, kDepthAgreeThreshold)) {
			result = SrcShadowTexture[r.otherPx];  // surfaces agree; transfer is exact
		} else {
			r.valid = false;
			r.skipReason = 2;  // other-eye mask/sky or depth disagreement
		}
	}

#	ifdef DEBUG_DISOCCLUSION
	OutShadowTexture[dtid] = r.valid ? 1.0 : 0.0;  // measure disocclusion coverage
#	else
	OutShadowTexture[dtid] = result;
#	endif
}

#endif  // VR
