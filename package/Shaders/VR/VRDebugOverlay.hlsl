cbuffer Params : register(b0) {
    float InnerRadius;
    float MiddleRadius;
    float OuterRadius;
    float AspectRatio;
};

struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
};

float4 main(VS_OUTPUT input) : SV_TARGET
{
    float2 center = float2(0.5, 0.5);
    float2 delta = input.UV - center;

    // Correct for aspect ratio to ensure circles appear circular
    // We assume the settings radii (0..1) correspond to a normalized range where 1.0 touches the edge of the view height.
    delta.x *= AspectRatio;

    float dist = length(delta);

    // Map distance to normalized radius setting (0..1)
    // Assuming 1.0 radius corresponds to the edge of the view (distance 0.5 from center)
    float r = dist * 2.0;

    float thickness = 0.01; // Line thickness

    // Draw rings
    if (abs(r - InnerRadius) < thickness) {
        return float4(0.0, 1.0, 0.0, 0.8); // Green (Inner/Full Res boundary)
    }
    if (abs(r - MiddleRadius) < thickness) {
        return float4(1.0, 1.0, 0.0, 0.8); // Yellow (Middle/Half Res boundary)
    }
    if (abs(r - OuterRadius) < thickness) {
        return float4(1.0, 0.0, 0.0, 0.8); // Red (Outer/Quarter Res boundary)
    }

    return float4(0, 0, 0, 0); // Transparent
}
