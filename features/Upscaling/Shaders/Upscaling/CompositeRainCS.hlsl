#define COMPUTESHADER

#include "Common/Color.hlsli"

Texture2D<float4> RainLayer : register(t0);
RWTexture2D<float4> Scene : register(u0);

cbuffer CompositeData : register(b0)
{
	uint SceneLinear;
	uint3 Padding;
};

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 outputSize;
	Scene.GetDimensions(outputSize.x, outputSize.y);
	if (any(pixel >= outputSize))
		return;
	float4 rain = RainLayer.Load(int3(pixel, 0));
	if (!any(rain))
		return;
	float4 scene = Scene[pixel];
	float3 background = SceneLinear ? Color::SceneLinearToGamma(scene.rgb) : scene.rgb;
	float3 color = background * (1.0 - rain.a) + rain.rgb;
	scene.rgb = SceneLinear ? Color::SceneGammaToLinear(color) : color;
	scene.a = rain.a + scene.a * (1.0 - rain.a);
	Scene[pixel] = scene;
}
