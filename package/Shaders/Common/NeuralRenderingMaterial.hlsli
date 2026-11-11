#ifndef __NEURAL_RENDERING_MATERIAL_HLSLI__
#define __NEURAL_RENDERING_MATERIAL_HLSLI__

#include "NeuralRenderingCategory.hlsli"

namespace NeuralRenderingMaterial
{
	/** @brief Clamps a source tap within its eye before applying the stereo offset. */
	int2 SourcePixel(int2 pixel, uint2 size, uint eyeOffsetX)
	{
		return clamp(pixel, int2(0, 0), int2(size) - 1) + int2((int)eyeOffsetX, 0);
	}

	/** @brief Decodes one material sample into its neural protection weight. */
	float ProtectionAt(Texture2D<float2> categories, int2 pixel, float4 strengthsA, float4 strengthsB)
	{
		const uint category = NeuralRenderingCategory::Decode(categories[pixel].y);
		return 1.0 - NeuralRenderingCategory::CategoryStrength(category, strengthsA, strengthsB);
	}

	/** @brief Box-filters material protection using only samples within the source eye. */
	float Protection(Texture2D<float2> categories, int2 pixel, uint2 size, uint eyeOffsetX,
		uint edgeSoftness, float4 strengthsA, float4 strengthsB)
	{
		const int radius = (int)edgeSoftness;
		float sum = 0.0;
		if (radius == 0) {
			sum = ProtectionAt(categories, SourcePixel(pixel, size, eyeOffsetX), strengthsA, strengthsB);
		} else {
			for (int y = -radius; y <= radius; ++y) {
				for (int x = -radius; x <= radius; ++x)
					sum += ProtectionAt(categories, SourcePixel(pixel + int2(x, y), size, eyeOffsetX), strengthsA, strengthsB);
			}
			sum /= float((2 * radius + 1) * (2 * radius + 1));
		}
		return sum;
	}

	/** @brief Converts protection to a compositor weight at the provider mask's R8_UNORM precision. */
	float Selection(float protection)
	{
		return 1.0 - round(saturate(protection) * 255.0) / 255.0;
	}
}

#endif
