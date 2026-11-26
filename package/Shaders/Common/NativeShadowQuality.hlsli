#ifndef NATIVE_SHADOW_QUALITY_HLSLI
#define NATIVE_SHADOW_QUALITY_HLSLI

#ifdef __cplusplus
#	include <cstdint>
namespace DX12Native
{
	using uint = uint32_t;
#	define SHADOW_QUALITY_CONST inline constexpr
#	define SHADOW_QUALITY_FUNCTION inline constexpr
#else
#	define SHADOW_QUALITY_CONST static const
#	define SHADOW_QUALITY_FUNCTION
#endif

	// Keep the serialized IDs of the two checkerboard qualities.
	SHADOW_QUALITY_CONST uint kShadowQualityQuality = 1u;
	SHADOW_QUALITY_CONST uint kShadowOffloadingQuality = 2u;
	SHADOW_QUALITY_CONST uint kShadowQualityLow = 3u;
	SHADOW_QUALITY_CONST uint kOffloadingShaderIndex = 2u;
	SHADOW_QUALITY_CONST uint kShadowShaderCount = 3u;
	SHADOW_QUALITY_CONST float kOffloadingFadeRange = 0.66f;
	SHADOW_QUALITY_CONST float kOffloadingNearRadius = 16.0f;
	SHADOW_QUALITY_CONST float kOffloadingBlend = 0.40f;

	SHADOW_QUALITY_FUNCTION uint NativeShadowSanitizeRTQuality(uint quality)
	{
		return quality >= kShadowOffloadingQuality && quality <= 4u ? kShadowQualityLow : kShadowQualityQuality;
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowShaderIndex(bool offloading, uint quality)
	{
		return offloading ? kOffloadingShaderIndex : (NativeShadowSanitizeRTQuality(quality) == kShadowQualityLow ? 1u : 0u);
	}

	SHADOW_QUALITY_FUNCTION float NativeShadowEmitterMultiplier(uint quality)
	{
		return quality == kShadowQualityLow ? 0.20f : 0.65f;
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowCeilDivide(uint size, uint divisor)
	{
		return size / divisor + (size % divisor != 0u ? 1u : 0u);
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowQualityScale(uint quality)
	{
		return quality == kShadowOffloadingQuality || quality == kShadowQualityLow ? 2u : 1u;
	}

	SHADOW_QUALITY_FUNCTION bool NativeShadowCheckerboard(uint quality)
	{
		return quality == kShadowQualityQuality || quality == kShadowQualityLow;
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowCheckerboardOrigin(uint quality, uint frame)
	{
		return NativeShadowCheckerboard(quality) ? (frame & 1u) : 0u;
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowLogicalExtent(uint size, uint quality)
	{
		return NativeShadowCeilDivide(size, NativeShadowQualityScale(quality));
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowPhysicalWidth(uint logicalWidth, uint quality)
	{
		return NativeShadowCheckerboard(quality) ? NativeShadowCeilDivide(logicalWidth, 2u) : logicalWidth;
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowLogicalX(uint packedX, uint y, uint quality, uint origin)
	{
		return NativeShadowCheckerboard(quality) ? 2u * packedX + ((y ^ origin) & 1u) : packedX;
	}

	SHADOW_QUALITY_FUNCTION bool NativeShadowHasSample(uint x, uint y, uint quality, uint origin)
	{
		return !NativeShadowCheckerboard(quality) || ((x ^ y ^ origin) & 1u) == 0u;
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowPackedX(uint logicalX, uint quality)
	{
		return NativeShadowCheckerboard(quality) ? logicalX / 2u : logicalX;
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowRenderCoordinate(uint logical, uint extent, uint quality)
	{
		const uint scale = NativeShadowQualityScale(quality);
		const uint pixel = logical * scale + scale / 2u;
		return pixel < extent ? pixel : extent - 1u;
	}

#ifdef __cplusplus
}
#endif
#undef SHADOW_QUALITY_CONST
#undef SHADOW_QUALITY_FUNCTION
#endif
