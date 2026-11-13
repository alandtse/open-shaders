#include "Common/FrameBuffer.hlsli"
#include "DX12NativeScene/NativeShadowMask.hlsli"

RWTexture2D<float4> CameraRays : register(u0);
cbuffer Grid : register(b0)
{
	uint2 Size;
	uint2 NativeSize;
};

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= Size))
		return;
	float2 clip = float2(NativeShadowClip(pixel.x, Size.x), -NativeShadowClip(pixel.y, Size.y));
	float4 view = mul(FrameBuffer::CameraProjInverse[0], float4(clip, 0.5, 1));
	float3 direction = mul((float3x3)FrameBuffer::CameraViewInverse[0], view.xyz / view.w);
	float magnitude = dot(direction, direction);
	bool valid = all(isfinite(view)) && abs(view.w) > 1e-8 && all(isfinite(direction)) && magnitude > 1e-8;
	CameraRays[pixel] = valid ? float4(direction * rsqrt(magnitude), 1) : 0;
}
