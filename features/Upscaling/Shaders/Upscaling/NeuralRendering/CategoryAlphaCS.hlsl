#include "Common/NeuralRenderingMaterial.hlsli"

cbuffer CategoryAlpha : register(b0)
{
	uint Width;
	uint Height;
	uint EyeOffsetX;
	uint EdgeSoftness;
	float4 StrengthsA;  // None, Skin, Hair, Eyes
	float4 StrengthsB;  // Foliage, Landscape, Cloth, Metal
};

Texture2D<float2> Masks2Texture : register(t0);  // r vertex AO, g material category (R16G16_UNORM)
RWTexture2D<float> CategoryAlphaOutput : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	CategoryAlphaOutput[id.xy] = NeuralRenderingMaterial::Protection(Masks2Texture, int2(id.xy),
		uint2(Width, Height), EyeOffsetX, EdgeSoftness, StrengthsA, StrengthsB);
}
