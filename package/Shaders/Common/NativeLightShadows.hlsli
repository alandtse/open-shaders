#ifndef NATIVE_LIGHT_SHADOWS_HLSLI
#define NATIVE_LIGHT_SHADOWS_HLSLI

#include "Common/NativeLightShadowSlot.hlsli"

// Ray-traced static visibility per light, one slice per traced light, written by DX12 Native Scene
// at reduced resolution before forward lighting. DX12NativeScene::kLightShadowRegister binds it.
Texture2DArray<unorm float> NativeLightShadowSlices : register(t57);
// The previous frame's slices: its checkerboard phase and emitter samples complement this frame's.
Texture2DArray<unorm float> PreviousNativeLightShadowSlices : register(t58);

namespace NativeLightShadows
{
	/** Visibility of one traced light at a render-target pixel, averaged with the previous frame's slice when it has one; 1 is lit. */
	float Visibility(uint packed, float2 pixel)
	{
		const uint slot = LightShadowCurrent(packed), previousSlot = LightShadowPrevious(packed);
		uint width, height, slices, previousWidth, previousHeight, previousSlices;
		NativeLightShadowSlices.GetDimensions(width, height, slices);
		PreviousNativeLightShadowSlices.GetDimensions(previousWidth, previousHeight, previousSlices);
		const bool hasCurrent = slot < slices && width != 0 && height != 0;
		const bool hasPrevious = previousSlot < previousSlices && previousWidth != 0 && previousHeight != 0 &&
		                         (!hasCurrent || (previousWidth == width && previousHeight == height));
		if (!hasCurrent && !hasPrevious)
			return 1.0;
		const uint2 size = hasCurrent ? uint2(width, height) : uint2(previousWidth, previousHeight);
		const float2 renderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
		const uint2 texel = min(uint2(pixel * float2(size) / max(renderSize, 1.0)), size - 1);
		const float current = hasCurrent ? NativeLightShadowSlices.Load(int4(texel, slot, 0)) : 1.0;
		const float previous = hasPrevious ? PreviousNativeLightShadowSlices.Load(int4(texel, previousSlot, 0)) : 1.0;
		return hasCurrent && hasPrevious ? 0.5 * (current + previous) : (hasCurrent ? current : previous);
	}
}

#endif
