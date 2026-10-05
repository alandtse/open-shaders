#ifndef COMMON_NEURAL_RENDERING_CATEGORY
#define COMMON_NEURAL_RENDERING_CATEGORY

/**
 * @brief Material category stored in the green channel of the deferred Masks2 target.
 *
 * Masks2 is R16G16_UNORM: r keeps 1 - vertexAO and g holds a category code. The target is alpha
 * blended, so a blended pixel carries a mix of two codes. The codes are spaced so that no code is
 * the midpoint of two others, and Decode accepts only a value within a small window of an exact
 * code, so a blend decodes to None instead of a neighbouring material.
 */
namespace NeuralRenderingCategory
{
	static const uint None = 0;
	static const uint Skin = 1;
	static const uint Hair = 2;
	static const uint Eyes = 3;
	static const uint Foliage = 4;
	static const uint Landscape = 5;
	static const uint Count = 6;

	/** Category code in 1/255 units, indexed by category id. */
	static const uint kCodes[Count] = { 0, 25, 75, 100, 225, 250 };

	/** Largest distance from an exact code, in 1/255 units, that still decodes as that category. */
	static const float kCodeTolerance = 0.1;

	/**
	 * @brief Encodes a category id into the UNORM value Masks2.g stores.
	 * @param category One of the category ids above; an id outside the range encodes as None.
	 * @return The value to write to the green channel.
	 */
	float Encode(uint category)
	{
		return kCodes[category < Count ? category : None] / 255.0;
	}

	/**
	 * @brief Decodes a Masks2.g value back into a category id.
	 * @param encoded The green channel read from Masks2.
	 * @return The category id, or None when the value is not within kCodeTolerance of a category code.
	 */
	uint Decode(float encoded)
	{
		const float code = encoded * 255.0;
		[unroll] for (uint category = Skin; category < Count; ++category)
		{
			if (abs(code - float(kCodes[category])) < kCodeTolerance)
				return category;
		}
		return None;
	}

	/**
	 * @brief NR strength for a category, in NeuralRenderingCategory id order.
	 * @param category A decoded category id (None..Landscape).
	 * @param low Strengths for None, Skin, Hair and Eyes in .x, .y, .z and .w.
	 * @param high Strengths for Foliage and Landscape in .x and .y.
	 * @return The category's strength; an unknown id returns the None strength.
	 */
	float CategoryStrength(uint category, float4 low, float2 high)
	{
		switch (category) {
		case Skin:
			return low.y;
		case Hair:
			return low.z;
		case Eyes:
			return low.w;
		case Foliage:
			return high.x;
		case Landscape:
			return high.y;
		default:
			return low.x;
		}
	}

	/**
	 * @brief Fixed debug colour for a category, used by the Neural Rendering category visualisation.
	 * @param category A decoded category id; an unknown id renders as None.
	 * @return The debug colour.
	 */
	float3 DebugColor(uint category)
	{
		switch (category) {
		case Skin:
			return float3(1.0, 0.0, 0.0);
		case Hair:
			return float3(1.0, 0.5, 0.0);
		case Eyes:
			return float3(1.0, 1.0, 0.0);
		case Foliage:
			return float3(0.0, 1.0, 0.0);
		case Landscape:
			return float3(0.0, 1.0, 1.0);
		default:
			return float3(0.05, 0.05, 0.05);
		}
	}
}

#endif  // COMMON_NEURAL_RENDERING_CATEGORY
