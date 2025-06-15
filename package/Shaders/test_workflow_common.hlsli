// Dummy HLSL include file to test CI workflow triggers
// This file should trigger the shader-validation job when modified in a PR

#ifndef TEST_WORKFLOW_COMMON_HLSLI
#define TEST_WORKFLOW_COMMON_HLSLI

// Common constants for testing
static const float PI = 3.14159265359;
static const float TWO_PI = 6.28318530718;
static const float HALF_PI = 1.57079632679;

// Test utility structures
struct TestLightData
{
    float3 position;
    float range;
    float3 color;
    float intensity;
    float3 direction;
    float spotAngle;
};

struct TestMaterialData
{
    float3 albedo;
    float metallic;
    float3 normal;
    float roughness;
    float3 emission;
    float occlusion;
};

// Utility functions for testing
float3 sampleNormalMap(Texture2D normalTexture, SamplerState normalSampler, float2 uv)
{
    float3 normal = normalTexture.Sample(normalSampler, uv).rgb;
    return normalize(normal * 2.0 - 1.0);
}

float calculateAttenuation(float distance, float range)
{
    float attenuation = 1.0 - saturate(distance / range);
    return attenuation * attenuation;
}

float3 calculateFresnelSchlick(float3 F0, float cosTheta)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float distributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / denom;
}

float geometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Test noise functions
float random(float2 uv)
{
    return frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453123);
}

float noise(float2 uv)
{
    float2 i = floor(uv);
    float2 f = frac(uv);

    float a = random(i);
    float b = random(i + float2(1.0, 0.0));
    float c = random(i + float2(0.0, 1.0));
    float d = random(i + float2(1.0, 1.0));

    float2 u = f * f * (3.0 - 2.0 * f);

    return lerp(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

#endif // TEST_WORKFLOW_COMMON_HLSLI

// This file exists to test:
// - HLSL include file change detection in .github/workflows/build.yaml
// - shader-validation job triggering
// - File patterns: **.hlsli, package/Shaders/**
