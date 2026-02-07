Texture2D<uint> MaskTexture : register(t0);

struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
};

float4 main(VS_OUTPUT input) : SV_TARGET
{
    uint w, h;
    MaskTexture.GetDimensions(w, h);

    int3 x = int3(input.UV.x * w, input.UV.y * h, 0);
    uint maskValue = MaskTexture.Load(x);

    // RDM Values:
    // 0 = Full Quality (Keep All)
    // 1 = Half Quality (1/2 density)
    // 2 = Quarter Quality (1/4 density)
    // 3 = Edge Transition (1/16 density) <- Reduces white outlines
    // 4 = Full Cull

    if (maskValue == 0) return float4(0.0, 1.0, 0.0, 0.1);     // Green (Full)
    if (maskValue == 1) return float4(0.0, 0.0, 1.0, 0.2);     // Blue (Half)
    if (maskValue == 2) return float4(1.0, 1.0, 0.0, 0.3);     // Yellow (Quarter)
    if (maskValue == 3) return float4(1.0, 0.5, 0.0, 0.4);     // Orange (Edge 1/16)
    if (maskValue == 4) return float4(1.0, 0.0, 0.0, 0.5);     // Red (Cull)

    return float4(0, 0, 0, 0);
}
