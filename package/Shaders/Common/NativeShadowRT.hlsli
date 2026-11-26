#ifndef NATIVE_SHADOW_RT_HLSLI
#define NATIVE_SHADOW_RT_HLSLI
#include "NativeShadowOffload.hlsli"

#ifdef __cplusplus
namespace DX12Native
{
#	define RT_CONST inline constexpr
#	define RT_FUNC inline constexpr
#else
#	define RT_CONST static const
#	define RT_FUNC
#endif

	RT_CONST uint kShadowQualityQuality = 1u;
	RT_CONST uint kShadowQualityPerformance = 3u;
	RT_CONST uint kShadowQualityOffloading = 2u;
	RT_CONST float kRayTracedReach = 1.25f;
	RT_CONST float kRayTracedFade = 0.36f;
	RT_CONST float kRayTracedBlend = 0.55f;
	RT_CONST uint kShadowVariantCount = 3u;

	RT_FUNC uint SanitizeRTQuality(uint quality)
	{
		return quality == kShadowQualityPerformance ? kShadowQualityPerformance : kShadowQualityQuality;
	}
	RT_FUNC uint ShadowVariant(uint quality)
	{
		return quality == kShadowQualityOffloading ? 0u : (quality == kShadowQualityPerformance ? 2u : 1u);
	}
	RT_FUNC uint ShadowScale(uint quality)
	{
		return quality == kShadowQualityQuality ? 1u : 2u;
	}
	RT_FUNC uint ShadowLogicalExtent(uint extent, uint quality)
	{
		return extent / ShadowScale(quality) + (extent % ShadowScale(quality) != 0u ? 1u : 0u);
	}
	RT_FUNC uint ShadowPhysicalWidth(uint logicalWidth, uint quality)
	{
		return quality == kShadowQualityOffloading ? logicalWidth : NativeShadowOffloadExtent(logicalWidth);
	}
	RT_FUNC uint ShadowLogicalX(uint packedX, uint y, uint phase)
	{
		return 2u * packedX + ((y ^ phase) & 1u);
	}
	RT_FUNC uint ShadowRenderCoordinate(uint logical, uint extent, uint quality)
	{
		const uint scale = ShadowScale(quality);
		const uint pixel = logical * scale + scale / 2u;
		return pixel < extent ? pixel : extent - 1u;
	}
	RT_FUNC uint ShadowTemporalFrames(uint quality)
	{
		return quality == kShadowQualityPerformance ? 3u : (quality == kShadowQualityQuality ? 2u : 1u);
	}
	RT_FUNC uint ShadowReconstructionRadius()
	{
		return 1u;
	}
	RT_FUNC bool ShadowFilterDirectSamples(uint quality)
	{
		return quality == kShadowQualityPerformance;
	}
	RT_FUNC bool ShadowClampHistory(uint quality)
	{
		return quality == kShadowQualityPerformance;
	}
	RT_FUNC float ShadowEmitterMultiplier(uint quality)
	{
		return quality == kShadowQualityOffloading ? kOffloadingEmitterMultiplier :
			(quality == kShadowQualityPerformance ? 0.15f : 0.65f);
	}
#ifdef __cplusplus
}
#endif
#undef RT_CONST
#undef RT_FUNC
#endif
