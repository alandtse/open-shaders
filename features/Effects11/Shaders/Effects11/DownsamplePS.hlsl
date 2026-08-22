Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	// SourceTexture is already single-eye by the time it reaches here (see
	// EffectManager::RefreshEyeSourceTexture), so no VR-specific handling is needed.
	return SourceTexture.SampleLevel(LinearSampler, input.txcoord0.xy, 0);
}
