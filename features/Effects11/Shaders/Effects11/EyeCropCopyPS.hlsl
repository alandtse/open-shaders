Texture2D<float4> SourceTexture : register(t0);

// Pixel offset (not UV) into the packed side-by-side source: eyeIndex * halfWidth.
// A raw pixel-space Load, not a Sample, so this is an exact crop with no resampling.
cbuffer EyeCropParams : register(b0)
{
	uint EyeOffsetX;
}

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	return SourceTexture.Load(int3(int(input.pos.x) + int(EyeOffsetX), int(input.pos.y), 0));
}
