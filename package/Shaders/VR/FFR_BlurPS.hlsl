// FFR Distance-Based Blur Shader
// Inspired by Skyrim-Upscaler (Puredark) blur_ps.hlsl
// Applies blur based on distance from foveation center

cbuffer BlurParams : register(b0)
{
    float2 InvResolution;       // 1.0 / resolution
    float2 ProjectionCenterL;   // Left eye center (normalized 0-1)
    float2 ProjectionCenterR;   // Right eye center (normalized 0-1)
    float InnerRadius;          // Start blurring beyond this
    float BlurIntensity;        // Multiplier for blur strength
    float HalfWidth;            // 0.5 (eye boundary)
    float3 Pad;
};

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
Texture2D SourceTex : register(t0);
Texture2D<float> DepthTex : register(t1);

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

// Helper: Check if pixel is sky (culled by RDM)
// Don't blur sky pixels to avoid spreading sky color onto geometry
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

// Simple 3x3 Gaussian blur with variable radius
// Adapted from Skyrim-Upscaler's FastBlur
float4 FastBlur(float2 uv, float2 stepSize, float blurRadius)
{
    float4 result = 0;

    // 3x3 kernel
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 offset = float2(x, y) * stepSize * blurRadius;
            result += SourceTex.SampleLevel(LinearSampler, uv + offset, blurRadius);
        }
    }

    return result / 9.0f;
}

float4 main(VS_OUTPUT input) : SV_TARGET
{
    float2 uv = input.TexCoord;

    // Load source pixel
    float4 sourceColor = SourceTex.Sample(LinearSampler, uv);

    // Don't blur sky pixels - just pass through
    if (IsSkyPixel(sourceColor, uv)) {
        return sourceColor;
    }

    // Determine which eye and get center
    float2 center = (uv.x < HalfWidth) ? ProjectionCenterL : ProjectionCenterR;

    // Calculate distance from foveation center
    float2 toCenter = uv - center;
    float distSq = dot(toCenter, toCenter);
    float dist = sqrt(distSq);

    // Only blur beyond inner radius (foveated region)
    if (dist < InnerRadius) {
        // No blur in high-quality zone
        return sourceColor;
    }

    // Calculate blur intensity based on distance squared
    // Formula from Skyrim-Upscaler: blur = k * r^2 * intensity
    float blurAmount = 18.0f * distSq * BlurIntensity;

    // Clamp to reasonable range (0-3 LOD levels)
    blurAmount = clamp(blurAmount, 0.0f, 3.0f);

    // Apply distance-based blur
    return FastBlur(uv, InvResolution, blurAmount);
}
