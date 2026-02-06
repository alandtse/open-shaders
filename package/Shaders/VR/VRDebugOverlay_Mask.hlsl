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
    // 0 = 1x1 (Full Quality)
    // 1 = 1x2 (Half Quality)
    // 2 = 2x2 (Quarter Quality)
    // 3 = 4x4 / Cull (Lowest Quality)

    if (maskValue == 0) return float4(0.0, 1.0, 0.0, 0.1); // Green (1x1)
    if (maskValue == 1) return float4(0.0, 0.0, 1.0, 0.2); // Blue (1x2)
    if (maskValue == 2) return float4(1.0, 1.0, 0.0, 0.3); // Yellow (2x2)
    if (maskValue == 3) return float4(1.0, 0.0, 0.0, 0.4); // Red (4x4/Cull)

    return float4(0, 0, 0, 0);
}
