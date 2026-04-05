// Composites two per-eye R8 HMD masks into a single full-stereo R8 mask at t21 resolution.
//
// Output layout matches the SBS render-res stereo buffer: left eye at [0, renderW-1],
// right eye at [renderW, 2*renderW-1].  This matches the coordinate space seen by
// deferred compute shaders, so IsHiddenPixel can sample directly without any remapping.
//
// Only needs to run when the resolution or HMD mesh changes (not every frame).

Texture2D<float> EyeMask0 : register(t0);  // left-eye  R8 mask at renderW x renderH
Texture2D<float> EyeMask1 : register(t1);  // right-eye R8 mask at renderW x renderH

RWTexture2D<float> StereoMask : register(u0);  // output: renderW*2 x renderH

cbuffer BuildStereoMaskCB : register(b0)
{
	uint renderW;  // per-eye rendered width
	uint renderH;  // per-eye rendered height
	uint pad0;
	uint pad1;
};

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	uint w, h;
	StereoMask.GetDimensions(w, h);
	if (DTid.x >= w || DTid.y >= h)
		return;

	// Right eye starts immediately after left eye, matching SBS render-res layout.
	uint eyeIdx = (DTid.x >= renderW) ? 1 : 0;
	uint eyeX = DTid.x - eyeIdx * renderW;

	StereoMask[DTid.xy] = eyeIdx == 0 ? EyeMask0[int2(eyeX, DTid.y)] : EyeMask1[int2(eyeX, DTid.y)];
}
