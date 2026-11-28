#ifndef NATIVE_LIGHT_SHADOW_SLOT_HLSLI
#define NATIVE_LIGHT_SHADOW_SLOT_HLSLI

// One word per light carries its ray-traced shadow, so no shared light flag bit is spent on it:
// bits 0-7 this frame's slice, 8-15 the previous frame's slice (kNoLightShadowSlice when absent), 16+ flags.
#ifdef __cplusplus
#	include <cstdint>
namespace DX12Native
{
	using uint = uint32_t;
#	define LIGHT_SHADOW_CONST inline constexpr
#	define LIGHT_SHADOW_FUNCTION inline constexpr
#else
#	define LIGHT_SHADOW_CONST static const
#	define LIGHT_SHADOW_FUNCTION
#endif

	LIGHT_SHADOW_CONST uint kNoLightShadowSlice = 0xFFu;
	LIGHT_SHADOW_CONST uint kNoLightShadow = 0xFFFFu;
	/** The traced term follows the SCM slot's swap fade (shadow offload traces a light only while it has a slot). */
	LIGHT_SHADOW_CONST uint kLightShadowFadesWithSlot = 1u << 16u;

	LIGHT_SHADOW_FUNCTION uint PackLightShadow(uint current, uint previous, uint flags)
	{
		return (current & 0xFFu) | ((previous & 0xFFu) << 8u) | (flags & ~0xFFFFu);
	}
	LIGHT_SHADOW_FUNCTION uint LightShadowCurrent(uint packed) { return packed & 0xFFu; }
	LIGHT_SHADOW_FUNCTION uint LightShadowPrevious(uint packed) { return (packed >> 8u) & 0xFFu; }
	LIGHT_SHADOW_FUNCTION bool HasLightShadow(uint packed) { return (packed & 0xFFFFu) != kNoLightShadow; }

#ifdef __cplusplus
}
#endif
#undef LIGHT_SHADOW_CONST
#undef LIGHT_SHADOW_FUNCTION
#endif
