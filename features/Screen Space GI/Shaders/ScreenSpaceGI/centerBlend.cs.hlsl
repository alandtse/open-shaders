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
	const uint2 dispatchOffset = uint2(CenterDispatchOffsetX, CenterDispatchOffsetY);
	const uint2 dispatchSize = uint2(CenterDispatchSizeX, CenterDispatchSizeY);
	if (any(dtid >= dispatchSize))
		return;
	const uint2 pxCoord = dtid + dispatchOffset;

	const float2 uv = (pxCoord + 0.5) * RcpFrameDim;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float blendWeight = GetCenterFullMaskWeight(uv, eyeIndex);

	const half baseAo = srcBaseAo[pxCoord];
	const half4 baseIlY = srcBaseIlY[pxCoord];
	const half2 baseIlCoCg = srcBaseIlCoCg[pxCoord];
	const half4 baseGiSpec = srcBaseGiSpecular[pxCoord];

	// Periphery path: no center contribution, so avoid center texture reads.
	if (blendWeight <= 0.0) {
		outAo[pxCoord] = baseAo;
		outIlY[pxCoord] = baseIlY;
		outIlCoCg[pxCoord] = baseIlCoCg;
		outGiSpecular[pxCoord] = baseGiSpec;
		return;
	}

	const half centerAo = srcCenterAo[pxCoord];
	const half4 centerIlY = srcCenterIlY[pxCoord];
	const half2 centerIlCoCg = srcCenterIlCoCg[pxCoord];
	const bool fullCenter = (blendWeight >= 1.0);

	if (fullCenter) {
		outAo[pxCoord] = centerAo;
		outIlY[pxCoord] = centerIlY;
		outIlCoCg[pxCoord] = centerIlCoCg;
	} else {
		outAo[pxCoord] = lerp(baseAo, centerAo, blendWeight);
		outIlY[pxCoord] = lerp(baseIlY, centerIlY, blendWeight);
		outIlCoCg[pxCoord] = lerp(baseIlCoCg, centerIlCoCg, blendWeight);
	}
#ifdef GI_SPECULAR
	const half4 centerGiSpec = srcCenterGiSpecular[pxCoord];
	outGiSpecular[pxCoord] = lerp(baseGiSpec, centerGiSpec, blendWeight);
#else
	outGiSpecular[pxCoord] = baseGiSpec;
#endif
}
