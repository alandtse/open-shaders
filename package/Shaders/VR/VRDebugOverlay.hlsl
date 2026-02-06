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
    float2 uv = input.UV;
    float2 center;

    // Determine which eye we are in and set center accordingly
    // Skyrim VR: Left Eye [0, 0.5], Right Eye [0.5, 1.0]
    if (uv.x < 0.5) {
        center = float2(0.25, 0.5);
    } else {
        center = float2(0.75, 0.5);
    }

    float2 delta = uv - center;

    // Correct for aspect ratio of the *single eye view*
    // The AspectRatio passed in should be the full texture aspect ratio (Width/Height)
    // We want to normalize the x-distance so that the circles are circular relative to the eye view
    // Eye Width is 0.5 * Total Width.
    // So we scale x by 2 * AspectRatio?

    // Let's think in "eye-space" where U ranges 0..1 per eye.
    // dx = (uv.x - center.x) * 2.0; // range -0.5 to 0.5 -> -1 to 1? No.
    // If uv.x is 0.25 (center), dx is 0.
    // If uv.x is 0.0 (edge), dx is -0.25.

    // We want to calculate physical distance.
    // Vertical distance: delta.y (ranges -0.5 to 0.5)
    // Horizontal distance: delta.x (ranges -0.25 to 0.25)

    // If we scale delta.x by the full aspect ratio (W/H):
    // delta.x * (W/H) = (x_dist/W) * (W/H) = x_dist/H
    // delta.y = y_dist/H

    // So delta.x * AspectRatio gives us distance in terms of Height units.
    // This is correct for drawing circles.

    delta.x *= AspectRatio;

    float dist = length(delta);

    // Map distance to normalized radius setting (0..1)
    // The radius setting 1.0 corresponds to "touching the edge of the view height"
    // The edge of view height is at delta.y = 0.5.
    // So if dist = 0.5, we want r = 1.0.
    // Thus r = dist * 2.0.

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
