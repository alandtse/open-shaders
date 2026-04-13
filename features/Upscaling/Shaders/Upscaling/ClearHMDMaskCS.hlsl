// Zeros color in the HMD hidden area for a single eye region.
// Prevents DLSS/FSR from temporally accumulating the engine's sky/ambient clear color
// into visible pixels during head movement ("light blue border" ghosting).
// depth == 0.0 is the unrendered/hidden area value (Skyrim reversed-Z: far plane = 0).
// DepthIn can be a packed stereo depth buffer or another depth source. The shader
// supports direct eye-local addressing or scaled color->depth coordinate mapping.

cbuffer ClearHMDMaskCB : register(b0)
{
	uint DepthOffsetX;
	uint ColorOffsetX;
	uint DepthOffsetY;
	uint ColorOffsetY;
	uint DepthWidth;
	uint DepthHeight;
	uint ColorWidth;
	uint ColorHeight;
};

Texture2D<float> DepthIn : register(t0);
RWTexture2D<float4> ColorInOut : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (dispatchID.x >= ColorWidth || dispatchID.y >= ColorHeight)
		return;

	uint colorTexWidth, colorTexHeight;
	ColorInOut.GetDimensions(colorTexWidth, colorTexHeight);

	uint2 colorPos = dispatchID.xy + uint2(ColorOffsetX, ColorOffsetY);
	if (colorPos.x >= colorTexWidth || colorPos.y >= colorTexHeight)
		return;

	uint depthTexWidth, depthTexHeight;
	DepthIn.GetDimensions(depthTexWidth, depthTexHeight);

	uint2 depthPos;
	if (DepthWidth > 0 && DepthHeight > 0 && ColorWidth > 0 && ColorHeight > 0) {
		depthPos = uint2(
			(dispatchID.x * DepthWidth) / ColorWidth,
			(dispatchID.y * DepthHeight) / ColorHeight) +
			uint2(DepthOffsetX, DepthOffsetY);
	} else {
		depthPos = dispatchID.xy + uint2(DepthOffsetX, DepthOffsetY);
	}

	if (depthPos.x >= depthTexWidth || depthPos.y >= depthTexHeight)
		return;

	if (DepthIn[depthPos] == 0.0)
		ColorInOut[colorPos] = float4(0.0, 0.0, 0.0, 0.0);
}
