#ifndef NATIVE_COMPOSITE_RASTER_CHARACTER_HLSLI
#define NATIVE_COMPOSITE_RASTER_CHARACTER_HLSLI

// RasterCharacterBounds is a per-frame, camera-relative StructuredBuffer<float4>,
// provided by the native composite. It is not an exact per-triangle stencil:
// the depth-reconstructed receiver must fall inside an observed skinned bound.
// The mask intentionally fails open to untouched raster when a frame is absent
// or its camera/depth is invalid, rather than borrowing a stale character pose.
bool IsRasterCharacterReceiver(uint2 pixel, float depth, uint2 size,
    row_major float4x4 inverseProjection, row_major float4x4 inverseView,
    uint characterCount)
{
    if (characterCount == 0u || !isfinite(depth)) return false;
    float2 clip = (float2(pixel) + 0.5f) * (2.0f / float2(size)) - 1.0f;
    clip.y = -clip.y;
    float4 cameraView = mul(inverseProjection, float4(clip, depth, 1.0f));
    if (!all(isfinite(cameraView)) || abs(cameraView.w) <= 1e-8f) return false;
    float3 cameraRelative = mul((float3x3)inverseView,
        cameraView.xyz / cameraView.w);
    if (!all(isfinite(cameraRelative))) return false;
    [loop] for (uint i = 0; i < min(characterCount, 64u); ++i) {
        float4 region = RasterCharacterBounds[i];
        float3 difference = cameraRelative - region.xyz;
        if (isfinite(region.w) && region.w > 0.0f &&
            dot(difference, difference) <= region.w * region.w) return true;
    }
    return false;
}
#endif
