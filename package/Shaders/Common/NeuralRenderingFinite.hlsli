#ifndef NEURAL_RENDERING_FINITE_HLSLI
#define NEURAL_RENDERING_FINITE_HLSLI

namespace NeuralRenderingFinite
{
	static const uint kExponentMask = 0x7F800000u;

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
}

#endif
