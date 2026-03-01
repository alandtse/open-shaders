#include "Common/VR.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<half> srcBaseAo : register(t0);
Texture2D<half4> srcBaseIlY : register(t1);
Texture2D<half2> srcBaseIlCoCg : register(t2);
Texture2D<half4> srcBaseGiSpecular : register(t3);

Texture2D<half> srcCenterAo : register(t4);
Texture2D<half4> srcCenterIlY : register(t5);
Texture2D<half2> srcCenterIlCoCg : register(t6);
Texture2D<half4> srcCenterGiSpecular : register(t7);

RWTexture2D<half> outAo : register(u0);
RWTexture2D<half4> outIlY : register(u1);
RWTexture2D<half2> outIlCoCg : register(u2);
RWTexture2D<half4> outGiSpecular : register(u3);

float GetCenterFullMaskWeight(float2 stereoUv, uint eyeIndex)
{
	float2 eyeUv = Stereo::ConvertFromStereoUV(stereoUv, eyeIndex);
	float halfSize = saturate(CenterFullResMaskScale) * 0.5;
	float2 outside = abs(eyeUv - 0.5) - halfSize.xx;
	float distanceOutside = max(outside.x, outside.y);
	return 1.0 - smoothstep(0.0, max(CenterFullResMaskFeather, 1e-4), distanceOutside);
}

[numthreads(8, 8, 1)] void main(const uint2 dtid : SV_DispatchThreadID)
{
	if (any(dtid >= uint2(FrameDim)))
		return;

	const float2 uv = (dtid + 0.5) * RcpFrameDim;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float blendWeight = GetCenterFullMaskWeight(uv, eyeIndex);

	const half baseAo = srcBaseAo[dtid];
	const half4 baseIlY = srcBaseIlY[dtid];
	const half2 baseIlCoCg = srcBaseIlCoCg[dtid];
	const half4 baseGiSpec = srcBaseGiSpecular[dtid];

	// Periphery path: no center contribution, so avoid center texture reads.
	if (blendWeight <= 0.0) {
		outAo[dtid] = baseAo;
		outIlY[dtid] = baseIlY;
		outIlCoCg[dtid] = baseIlCoCg;
		outGiSpecular[dtid] = baseGiSpec;
		return;
	}

	const half centerAo = srcCenterAo[dtid];
	const half4 centerIlY = srcCenterIlY[dtid];
	const half2 centerIlCoCg = srcCenterIlCoCg[dtid];
	const bool fullCenter = (blendWeight >= 1.0);

	if (fullCenter) {
		outAo[dtid] = centerAo;
		outIlY[dtid] = centerIlY;
		outIlCoCg[dtid] = centerIlCoCg;
	} else {
		outAo[dtid] = lerp(baseAo, centerAo, blendWeight);
		outIlY[dtid] = lerp(baseIlY, centerIlY, blendWeight);
		outIlCoCg[dtid] = lerp(baseIlCoCg, centerIlCoCg, blendWeight);
	}
#ifdef GI_SPECULAR
	const half4 centerGiSpec = srcCenterGiSpecular[dtid];
	outGiSpecular[dtid] = lerp(baseGiSpec, centerGiSpec, blendWeight);
#else
	outGiSpecular[dtid] = baseGiSpec;
#endif
}
