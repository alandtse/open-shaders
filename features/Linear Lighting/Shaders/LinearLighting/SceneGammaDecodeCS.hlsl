#define COMPUTESHADER

#include "Common/Color.hlsli"

RWTexture2D<float4> SceneColor : register(u0);

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint width;
	uint height;
	SceneColor.GetDimensions(width, height);
	const uint2 dimensions = uint2(width, height);
	if (any(pixel >= dimensions))
		return;

	float4 color = SceneColor[pixel];
	color.rgb = Color::DecodeAuthoredColor(color.rgb);
	SceneColor[pixel] = color;
}
