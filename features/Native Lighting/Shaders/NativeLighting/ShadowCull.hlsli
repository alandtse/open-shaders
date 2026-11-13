#ifndef NATIVE_LIGHTING_SHADOW_CULL_HLSLI
#define NATIVE_LIGHTING_SHADOW_CULL_HLSLI
#ifdef __cplusplus
#	include <cmath>
namespace NativeLighting
{
#	define CULL_FINITE(value) std::isfinite(value)
#	define CULL_CONSTEXPR inline
#else
#	define CULL_FINITE(value) isfinite(value)
#	define CULL_CONSTEXPR
#endif
	CULL_CONSTEXPR bool CanCullShadow(bool authoritative, bool receiverMatches, float visibility, float validity, float distance, float instance)
	{
		return authoritative && receiverMatches && validity == 1.0f && CULL_FINITE(visibility) && visibility == 0.0f &&
		       CULL_FINITE(distance) && distance > 0.0f && CULL_FINITE(instance) && instance > 0.0f;
	}
	CULL_CONSTEXPR bool CanCullExperimentalMask(bool available, bool enabled, bool receiverMatches, float visibility, float validity, float distance, float instance, float threshold)
	{
		return available && enabled && receiverMatches && validity == 1.0f && CULL_FINITE(visibility) && CULL_FINITE(threshold) &&
		       threshold >= 0.0f && threshold <= 1.0f && visibility >= 0.0f && visibility <= threshold &&
		       CULL_FINITE(distance) && distance > 0.0f && CULL_FINITE(instance) && instance > 0.0f;
	}
#ifdef __cplusplus
}
#endif
#undef CULL_FINITE
#undef CULL_CONSTEXPR
#endif
