#ifndef NEURAL_RENDERING_FINITE_HLSLI
#define NEURAL_RENDERING_FINITE_HLSLI

namespace NeuralRenderingFinite
{
	static const uint kExponentMask = 0x7F800000u;
	static const float kMaximumHalf = 65504.0;

	/** @brief Classifies a value without floating-point optimizer assumptions. */
	bool IsFinite(float value)
	{
		return (asuint(value) & kExponentMask) != kExponentMask;
	}

	/** @brief Classifies both components independently. */
	bool2 IsFinite(float2 value)
	{
		return bool2(IsFinite(value.x), IsFinite(value.y));
	}

	/** @brief Classifies all three components independently. */
	bool3 IsFinite(float3 value)
	{
		return bool3(IsFinite(value.xy), IsFinite(value.z));
	}

	/** @brief Classifies all four components independently. */
	bool4 IsFinite(float4 value)
	{
		return bool4(IsFinite(value.xy), IsFinite(value.zw));
	}

	/** @brief Preserves source on failure and bounds accepted RGB for half-float storage. */
	float4 ResolveHalfOutput(float4 source, float3 candidate, bool valid)
	{
		return valid && all(IsFinite(source)) && all(IsFinite(candidate)) ?
		           float4(clamp(candidate, -kMaximumHalf, kMaximumHalf), source.a) :
		           source;
	}
}

#endif
