#ifndef NATIVE_LIGHT_SHADOWS_HLSLI
#define NATIVE_LIGHT_SHADOWS_HLSLI

// Ray-traced static visibility per light, one slice per traced light, written by DX12 Native Scene
// at reduced resolution before forward lighting. DX12NativeScene::kLightShadowRegister binds it.
Texture2DArray<unorm float> NativeLightShadowSlices : register(t57);
// The previous frame's slices: its checkerboard phase and emitter samples complement this frame's.
Texture2DArray<unorm float> PreviousNativeLightShadowSlices : register(t58);

namespace NativeLightShadows
{
	/** Visibility of one traced light at a render-target pixel, averaged with the previous frame's slice when it has one; 1 is lit. */
	float Visibility(uint slot, uint previousSlot, float2 pixel)
	{
		uint width, height, slices;
		NativeLightShadowSlices.GetDimensions(width, height, slices);
		if (slot >= slices || width == 0 || height == 0)
			return 1.0;
		const float2 renderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
		const uint2 texel = min(uint2(pixel * float2(width, height) / max(renderSize, 1.0)), uint2(width, height) - 1);
		const float current = NativeLightShadowSlices.Load(int4(texel, slot, 0));
		uint previousWidth, previousHeight, previousSlices;
		PreviousNativeLightShadowSlices.GetDimensions(previousWidth, previousHeight, previousSlices);
		if (previousSlot >= previousSlices || previousWidth != width || previousHeight != height)
			return current;
		return 0.5 * (current + PreviousNativeLightShadowSlices.Load(int4(texel, previousSlot, 0)));
	}
}

#endif
