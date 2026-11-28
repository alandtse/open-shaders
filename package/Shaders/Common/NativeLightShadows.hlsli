#ifndef NATIVE_LIGHT_SHADOWS_HLSLI
#define NATIVE_LIGHT_SHADOWS_HLSLI

// Ray-traced static visibility per light, one slice per traced light, written by DX12 Native Scene
// at reduced resolution before forward lighting. DX12NativeScene::kLightShadowRegister binds it.
Texture2DArray<unorm float> NativeLightShadowSlices : register(t57);

namespace NativeLightShadows
{
	/** Visibility of one traced light at a render-target pixel; 1 is lit. */
	float Visibility(uint slot, float2 pixel)
	{
		uint width, height, slices;
		NativeLightShadowSlices.GetDimensions(width, height, slices);
		if (slot >= slices || width == 0 || height == 0)
			return 1.0;
		const float2 renderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
		const uint2 texel = min(uint2(pixel * float2(width, height) / max(renderSize, 1.0)), uint2(width, height) - 1);
		return NativeLightShadowSlices.Load(int4(texel, slot, 0));
	}
}

#endif
