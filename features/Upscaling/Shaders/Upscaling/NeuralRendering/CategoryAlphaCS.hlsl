#include "Common/NeuralRenderingCategory.hlsli"

cbuffer CategoryAlpha : register(b0)
{
	uint Width;
	uint Height;
	uint EyeOffsetX;
	uint EdgeSoftness;
	float4 StrengthsA;  // None, Skin, Hair, Eyes
	float2 StrengthsB;  // Foliage, Landscape
};

Texture2D<float2> Masks2Texture : register(t0);  // r vertex AO, g material category (R16G16_UNORM)
RWTexture2D<float> CategoryAlphaOutput : register(u0);

// The lane is a protection value (1 restores the pixel from DLSSNR.Backbuffer, 0 applies NR fully): the inverse of the NR strength.
float Protection(int2 pixel)
{
	const uint category = NeuralRenderingCategory::Decode(Masks2Texture[pixel + int2(int(EyeOffsetX), 0)].y);
	return 1.0 - NeuralRenderingCategory::CategoryStrength(category, StrengthsA, StrengthsB);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	const int2 pixel = int2(id.xy);
	const int radius = int(EdgeSoftness);
	if (radius == 0) {
		CategoryAlphaOutput[id.xy] = Protection(pixel);
		return;
	}
	// The box average softens a material boundary; clamping the taps keeps a material that reaches
	// the eye border from being diluted by the missing neighbours outside it.
	const int2 last = int2(int(Width) - 1, int(Height) - 1);
	float sum = 0.0;
	for (int y = -radius; y <= radius; ++y) {
		for (int x = -radius; x <= radius; ++x)
			sum += Protection(clamp(pixel + int2(x, y), int2(0, 0), last));
	}
	CategoryAlphaOutput[id.xy] = sum / float((2 * radius + 1) * (2 * radius + 1));
}
