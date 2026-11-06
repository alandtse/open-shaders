#ifndef NATIVE_SHADOW_MASK_CONTRACT
#define NATIVE_SHADOW_MASK_CONTRACT
#ifdef __cplusplus
#include <cstdint>
namespace DX12Native {
using uint = uint32_t;
#define SHADOW_CONSTEXPR constexpr
#else
#define SHADOW_CONSTEXPR
#endif
SHADOW_CONSTEXPR float NativeShadowClip(uint pixel, uint size)
{
	return (float(pixel) + 0.5f) * 2.0f / float(size) - 1.0f;
}
SHADOW_CONSTEXPR bool NativeShadowValid(bool receiver, bool geometry, float distance)
{
	return receiver && geometry && distance > 0.0f && distance <= 60000.0f;
}
SHADOW_CONSTEXPR float NativeShadowLightWeight(float luminance, float attenuation, float facing)
{
	return luminance * attenuation * facing;
}
SHADOW_CONSTEXPR float NativeShadowWeightedVisibility(float visibleWeight, float totalWeight)
{
	return totalWeight > 1e-6f ? visibleWeight / totalWeight : 1.0f;
}
SHADOW_CONSTEXPR float NativeShadowLightVisibility(bool visible, float cutoff, bool fade)
{
	return visible ? 1.0f : fade ? 1.0f - cutoff : 0.0f;
}
SHADOW_CONSTEXPR float NativeShadowSampleState(bool traced)
{
	return traced ? 1.0f : -1.0f;
}
#ifdef __cplusplus
}
#endif
#undef SHADOW_CONSTEXPR
#endif
