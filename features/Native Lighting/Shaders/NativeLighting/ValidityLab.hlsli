#ifndef NATIVE_LIGHTING_VALIDITY_LAB_HLSLI
#define NATIVE_LIGHTING_VALIDITY_LAB_HLSLI
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace NativeLighting {
#define LAB_FINITE(x) std::isfinite(x)
#define LAB_MAX(a, b) std::max(a, b)
#define LAB_INLINE inline
#else
#define LAB_FINITE(x) isfinite(x)
#define LAB_MAX(a, b) max(a, b)
#define LAB_INLINE
#endif
LAB_INLINE bool DepthMatches(bool rasterValid, float error, float distance, float absoluteTolerance, float relativeTolerance)
{
	return rasterValid && LAB_FINITE(error) && LAB_FINITE(distance) && distance > 0.0f &&
		LAB_FINITE(absoluteTolerance) && LAB_FINITE(relativeTolerance) && absoluteTolerance >= 0.0f && relativeTolerance >= 0.0f &&
		error >= 0.0f && error <= LAB_MAX(absoluteTolerance, distance * relativeTolerance);
}
#ifdef __cplusplus
}
#endif
#undef LAB_FINITE
#undef LAB_MAX
#undef LAB_INLINE
#endif
