// Dummy HLSL shader file to test CI workflow triggers
// This file should trigger the shader-validation job when modified in a PR

// Test vertex shader
struct VS_INPUT
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
    float4 color : COLOR;
};

struct VS_OUTPUT
{
    float4 position : SV_POSITION;
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
    float4 color : COLOR;
};

cbuffer TestConstants : register(b0)
{
    float4x4 worldMatrix;
    float4x4 viewMatrix;
    float4x4 projectionMatrix;
    float4 testColor;
    float testTime;
    float testIntensity;
    float2 testUVOffset;
};

// Test utility functions
float3 calculateWorldPosition(float3 localPos)
{
    return mul(float4(localPos, 1.0), worldMatrix).xyz;
}

float3 transformNormal(float3 normal)
{
    return normalize(mul(normal, (float3x3)worldMatrix));
}

// Main vertex shader
VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;

    // Transform position to world space
    float3 worldPos = calculateWorldPosition(input.position);

    // Transform to view space
    float4 viewPos = mul(float4(worldPos, 1.0), viewMatrix);

    // Transform to projection space
    output.position = mul(viewPos, projectionMatrix);

    // Pass through world position
    output.worldPos = worldPos;

    // Transform normal
    output.normal = transformNormal(input.normal);

    // Apply UV offset for animation testing
    output.texCoord = input.texCoord + testUVOffset;

    // Modulate color with test parameters
    output.color = input.color * testColor * testIntensity;

    return output;
}

// Test pixel shader
float4 PSMain(VS_OUTPUT input) : SV_Target
{
    // Simple lighting calculation
    float3 lightDir = normalize(float3(1.0, 1.0, -1.0));
    float ndotl = max(0.0, dot(input.normal, lightDir));

    // Animate color based on time
    float3 animatedColor = input.color.rgb * (0.5 + 0.5 * sin(testTime));

    // Combine lighting and color
    float3 finalColor = animatedColor * ndotl;

    return float4(finalColor, input.color.a);
}

// This file exists to test:
// - HLSL file change detection in .github/workflows/build.yaml
// - shader-validation job triggering
// - File patterns: **.hlsl, features/**/Shaders/**
