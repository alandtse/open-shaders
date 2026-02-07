// Radial Density Mask Reconstruction Filter
// Adapted from VRPerfKit (fholger/vrperfkit) - MIT License
// Based on Valve's Alex Vlachos "Advanced VR Rendering Performance" (GDC 2016)
// Bilinear approximation technique for reconstructing culled pixels

Texture2D<float4> SourceTex : register(t0);
Texture2D<float> DepthTex : register(t1);  // Depth buffer to detect sky pixels
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
RWTexture2D<float4> OutputTex : register(u0);

cbuffer ReconstructParams : register(b0)
{
    float2 InvResolution;      // 1.0 / render resolution
    float2 ProjectionCenterL;  // Left eye center (normalized 0-1)
    float2 ProjectionCenterR;  // Right eye center (normalized 0-1)
    float2 InvClusterRes;      // For 8x8 block calculations
    float3 Radius;             // Inner, Middle, Outer radii
    float EdgeRadius;          // Edge transition radius
    float HalfWidth;           // 0.5 (boundary between eyes)
    float3 Pad;
}

// Helper: Determine which eye based on UV
float2 GetProjectionCenter(float2 uv)
{
    return (uv.x < HalfWidth) ? ProjectionCenterL : ProjectionCenterR;
}

// Helper: Calculate distance to center for this pixel
float GetDistanceToCenter(uint2 pixelPos)
{
    float2 uv = float2(pixelPos) * InvResolution;
    float2 center = GetProjectionCenter(uv);
    float2 toCenter = (pixelPos >> 3u) * InvClusterRes - center; // 8x8 blocks
    return 2.0 * length(toCenter);
}

// Helper: Check if pixel is sky (culled by RDM)
// Skyrim sky color is #779baf (RGB: 119, 155, 175) or black (if upscaler masks it)
// Also check depth == 0.0 which indicates RDM culled this pixel
bool IsSkyPixel(float4 color, float2 uv)
{
    // Check depth - if 0.0 it was culled by RDM
    float depth = DepthTex.SampleLevel(PointSampler, uv, 0);
    if (depth < 0.01f) return true; // Near plane = culled

    // Check for sky blue color #779baf = (0.467, 0.608, 0.686)
    float3 skyColor = float3(119.0/255.0, 155.0/255.0, 175.0/255.0);
    float colorDist = distance(color.rgb, skyColor);
    if (colorDist < 0.1f) return true; // Close to sky color

    // Check for black (upscaler masked sky)
    if (length(color.rgb) < 0.05f) return true;

    return false;
}

// Valve's bilinear reconstruction for half-res (1x2 pattern)
// Reconstructs missing pixels using weighted bilinear sampling
float4 ReconstructHalfRes(uint2 pixelPos, uint2 halfPos)
{
    // Check if this is a missing pixel in the checkerboard pattern
    if ((halfPos.x & 0x01u) != (halfPos.y & 0x01u)) {
        // Missing pixel - reconstruct using bilinear interpolation
        float2 offset0, offset1;

        // Calculate offsets based on position within 2x2 block
        offset0.x = (pixelPos.x & 0x01) == 0 ? -0.5f : 1.5f;
        offset0.y = (pixelPos.y & 0x01) == 0 ? 0.75f : 0.25f;

        offset1.x = (pixelPos.x & 0x01) == 0 ? 0.75f : 0.25f;
        offset1.y = (pixelPos.y & 0x01) == 0 ? -0.5f : 1.5f;

        float2 uv0 = (float2(pixelPos) + offset0) * InvResolution;
        float2 uv1 = (float2(pixelPos) + offset1) * InvResolution;

        float4 sample0 = SourceTex.SampleLevel(LinearSampler, uv0, 0);
        float4 sample1 = SourceTex.SampleLevel(LinearSampler, uv1, 0);

        return sample0 * 0.5f + sample1 * 0.5f;
    } else {
        // Existing pixel - use bilinear filter for better quality
        float2 uv = float2(pixelPos);
        uv.x += (pixelPos.x & 0x01) == 0 ? 0.75f : 0.25f;
        uv.y += (pixelPos.y & 0x01) == 0 ? 0.75f : 0.25f;
        return SourceTex.SampleLevel(LinearSampler, uv * InvResolution, 0);
    }
}

// Reconstruction for quarter-res (2x2 pattern)
float4 ReconstructQuarterRes(uint2 pixelPos, uint2 halfPos)
{
    // Simple nearest-neighbor reconstruction for 2x2 blocks
    int2 offset;
    offset.x = (halfPos.x & 0x01u) == 0 ? 0 : -2;
    offset.y = (halfPos.y & 0x01u) == 0 ? 0 : -2;

    int2 srcPos = int2(pixelPos) + offset;
    return SourceTex.Load(int3(srcPos, 0));
}

// Reconstruction for edge transition (1/16 pattern - 4x4 blocks)
float4 ReconstructEdgeTransition(uint2 pixelPos, uint2 halfPos)
{
    // Even coarser reconstruction for 4x4 blocks
    int2 block = int2(halfPos) & 0x03;
    int2 offset = block * -2;

    int2 srcPos = int2(pixelPos) + offset;
    return SourceTex.Load(int3(srcPos, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixelPos = DTid.xy;
    uint2 halfPos = pixelPos >> 1u;
    float2 uv = float2(pixelPos) * InvResolution;

    // Load source pixel
    float4 sourceColor = SourceTex.Load(int3(pixelPos, 0));

    // Skip reconstruction for sky pixels - just pass through
    if (IsSkyPixel(sourceColor, uv)) {
        OutputTex[pixelPos] = sourceColor;
        return;
    }

    // Calculate distance to determine which reconstruction method to use
    float distToCenter = GetDistanceToCenter(pixelPos);

    // Inner zone: Full quality - no reconstruction needed
    if (distToCenter < Radius.x) {
        OutputTex[pixelPos] = sourceColor;
        return;
    }

    // Beyond edge: Full cull - no reconstruction (black or source as-is)
    if (distToCenter > EdgeRadius) {
        OutputTex[pixelPos] = sourceColor;
        return;
    }

    // Half-res zone: Use Valve's bilinear reconstruction
    if (distToCenter < Radius.y) {
        // Check if we're near the boundary (use high-quality filter near edges)
        if (distToCenter + 2.0 * InvClusterRes.x < Radius.y) {
            OutputTex[pixelPos] = ReconstructHalfRes(pixelPos, halfPos);
        } else {
            // Near boundary - use simpler reconstruction to avoid artifacts
            if ((halfPos.x & 0x01u) != (halfPos.y & 0x01u)) {
                int2 offset;
                offset.x = (halfPos.y & 0x01u) == 0 ? -2 : 2;
                offset.y = 0;
                OutputTex[pixelPos] = SourceTex.Load(int3(int2(pixelPos) + offset, 0));
            } else {
                OutputTex[pixelPos] = SourceTex.Load(int3(pixelPos, 0));
            }
        }
        return;
    }

    // Quarter-res zone: Simple reconstruction
    if (distToCenter < Radius.z) {
        OutputTex[pixelPos] = ReconstructQuarterRes(pixelPos, halfPos);
        return;
    }

    // Edge transition zone: Coarse reconstruction
    OutputTex[pixelPos] = ReconstructEdgeTransition(pixelPos, halfPos);
}
