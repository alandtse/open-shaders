#include "Common/SharedData.hlsli"

Texture2D<float2> TAAMask : register(t0);

Texture2D<float> DepthPreWater : register(t1);
Texture2D<float> DepthPostWater : register(t2);

RWTexture2D<float> ReactiveMask : register(u0);

#if defined(TRANSPARENCY_MASK)
RWTexture2D<float> TransparencyCompositionMask : register(u1);
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	float2 taaMask = TAAMask[dispatchID.xy];

	float reactiveMask = taaMask.x * 0.25 + sqrt(taaMask.y);

	float depthPreWater = SharedData::GetScreenDepth(DepthPreWater[dispatchID.xy]);
	float depthPostWater = SharedData::GetScreenDepth(DepthPostWater[dispatchID.xy]);

	float depthDifference = saturate(abs(depthPreWater - depthPostWater));

	float transparencyCompositionMask = depthDifference;

#if defined(TRANSPARENCY_MASK)
	ReactiveMask[dispatchID.xy] = reactiveMask;
	TransparencyCompositionMask[dispatchID.xy] = transparencyCompositionMask;
#else
	ReactiveMask[dispatchID.xy] = max(reactiveMask, transparencyCompositionMask);
#endif

}
