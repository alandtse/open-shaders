#ifndef __HIZ_REDUCTION_DEPENDENCY_HLSL__
#define __HIZ_REDUCTION_DEPENDENCY_HLSL__

// Reduction op for every level of a Hi-Z pyramid. Max is the default so a consumer that defines
// nothing keeps the original farthest-depth chain; define HIZ_REDUCTION_MIN for a nearest-depth one.
#ifdef HIZ_REDUCTION_MIN
#	define HIZ_REDUCE2(a, b) min(a, b)
#	define HIZ_REDUCE4(a, b, c, d) min(min(a, b), min(c, d))
// Stored depth is inside [0, 1], so folding the reduction's identity into an accumulator changes nothing.
#	define HIZ_REDUCTION_IDENTITY 1.0
#else
#	define HIZ_REDUCE2(a, b) max(a, b)
#	define HIZ_REDUCE4(a, b, c, d) max(max(a, b), max(c, d))
#	define HIZ_REDUCTION_IDENTITY 0.0
#endif

// Far plane. Stored depth is inside [0, 1], so this is the identity of the min reduction and, for max,
// the "nothing rendered here" value that can only under-report a reduction that a consumer tests against.
static const float HiZFarDepth = 1.0;

#endif  // __HIZ_REDUCTION_DEPENDENCY_HLSL__
