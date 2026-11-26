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

	SHADOW_QUALITY_CONST uint kShadowQualityFull = 0u;
	SHADOW_QUALITY_CONST uint kShadowQualityQuality = 1u;
	SHADOW_QUALITY_CONST uint kShadowQualityMedium = 2u;
	SHADOW_QUALITY_CONST uint kShadowQualityLow = 3u;
	SHADOW_QUALITY_CONST uint kShadowQualityPotato = 4u;
	SHADOW_QUALITY_CONST uint kShadowQualityCount = 5u;

	SHADOW_QUALITY_FUNCTION float NativeShadowEmitterMultiplier(uint quality)
	{
		switch (quality) {
		case kShadowQualityFull:
			return 1.00f;
		case kShadowQualityMedium:
			return 0.40f;
		case kShadowQualityLow:
			return 0.20f;
		case kShadowQualityPotato:
			return 0.0f;
		default:
			return 0.65f;
		}
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowCeilDivide(uint size, uint divisor)
	{
		return size / divisor + (size % divisor != 0u ? 1u : 0u);
	}

	SHADOW_QUALITY_FUNCTION uint NativeShadowQualityScale(uint quality)
	{
		return quality == kShadowQualityPotato ? 3u :
		       (quality == kShadowQualityMedium || quality == kShadowQualityLow ? 2u : 1u);
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
