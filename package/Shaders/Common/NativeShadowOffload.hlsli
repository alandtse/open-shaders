#ifndef NATIVE_SHADOW_OFFLOAD_HLSLI
#define NATIVE_SHADOW_OFFLOAD_HLSLI

#ifdef __cplusplus
#	include <cstdint>
namespace DX12Native
{
	using uint = uint32_t;
#	define SHADOW_OFFLOAD_CONST inline constexpr
#	define SHADOW_OFFLOAD_FUNCTION inline constexpr
#else
#	define SHADOW_OFFLOAD_CONST static const
#	define SHADOW_OFFLOAD_FUNCTION
#endif

	SHADOW_OFFLOAD_CONST float kOffloadingFadeRange = 0.66f;
	SHADOW_OFFLOAD_CONST float kOffloadingNearRadius = 16.0f;
	SHADOW_OFFLOAD_CONST float kOffloadingBlend = 0.40f;
	SHADOW_OFFLOAD_CONST float kOffloadingEmitterMultiplier = 0.30f;
	SHADOW_OFFLOAD_CONST uint kOffloadingScale = 2u;

	SHADOW_OFFLOAD_FUNCTION uint NativeShadowOffloadExtent(uint size)
	{
		return size / kOffloadingScale + (size % kOffloadingScale != 0u ? 1u : 0u);
	}

	SHADOW_OFFLOAD_FUNCTION uint NativeShadowOffloadRenderCoordinate(uint logical, uint extent)
	{
		const uint pixel = logical * kOffloadingScale + kOffloadingScale / 2u;
		return pixel < extent ? pixel : extent - 1u;
	}

#ifdef __cplusplus
}
#endif
#undef SHADOW_OFFLOAD_CONST
#undef SHADOW_OFFLOAD_FUNCTION
#endif
