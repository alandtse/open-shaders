#include "Common/DummyVSTexCoord.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState AlbedoSampler      : register(s0);
SamplerState DiffuseSampler     : register(s1);
SamplerState SpecularSampler    : register(s2);
SamplerState SAOSampler         : register(s3);
SamplerState FogSampler         : register(s4);
SamplerState DirDiffuseSampler  : register(s5);
SamplerState DirSpecularSampler : register(s6);
SamplerState ShadowMaskSampler  : register(s7);

Texture2D<float4> AlbedoTex      : register(t0);
Texture2D<float4> DiffuseTex     : register(t1);
Texture2D<float4> SpecularTex    : register(t2);
Texture2D<float4> SAOTex         : register(t3);
Texture2D<float4> FogTex         : register(t4);
Texture2D<float4> DirDiffuseTex  : register(t5);
Texture2D<float4> DirSpecularTex : register(t6);
Texture2D<float4> ShadowMaskTex  : register(t7);

// --- SSGI hook (matches C++ Prepass bindings) ---
#if defined(SCREEN_SPACE_GI)
Texture2D<float>  SSGI_AO       : register(t46); // R8 UNORM (0..1 occlusion amount)
Texture2D<float>  SSGI_IL_Y     : register(t47); // Y of YCoCg
Texture2D<float2> SSGI_IL_CoCg  : register(t48); // Co,Cg of YCoCg
// Optional specular GI buffer (enable in code if you use it)
// Texture2D<float4> SSGI_Specular : register(t49);
#endif

cbuffer PerGeometry : register(b2)
{
	float4 FogParam     : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 diffuse  = DiffuseTex.Sample(DiffuseSampler,  input.TexCoord);
	float4 specular = SpecularTex.Sample(SpecularSampler, input.TexCoord);
	float4 albedo   = AlbedoTex.Sample(AlbedoSampler,    input.TexCoord);

#	if defined(DIRECTIONALLIGHT)
	float4 dirDiffuse  = DirDiffuseTex.Sample(DirDiffuseSampler,   input.TexCoord);
	float4 dirSpecular = DirSpecularTex.Sample(DirSpecularSampler, input.TexCoord);
#	else
	float4 dirDiffuse  = 0;
	float4 dirSpecular = 0;
#	endif

#	if !defined(MENU)
	float  shadowMask = ShadowMaskTex.Sample(ShadowMaskSampler, input.TexCoord).x;
	float  saoFactor  = SAOTex.Sample(SAOSampler, input.TexCoord).x;
#	else
	float  shadowMask = 1;
	float  saoFactor  = 1;
#	endif

	// ---- SSGI integrate ----
	// Compose AO and IL from SSGI, if available.
#if defined(SCREEN_SPACE_GI)
	// AO from SSGI: assume 0=no occlusion, 1=full occlusion (as produced by our compute).
	// Convert to a multiplicative factor like SAO: (1 - ao).
	float ssgiAO = SSGI_AO.Sample(SAOSampler, input.TexCoord); // reuse point/linear doesn't matter for single texel
	float aoFactor = saturate(1.0 - ssgiAO);

	// Combine vanilla SAO with SSGI AO multiplicatively.
	float saoCombined = saturate(saoFactor * aoFactor);

	// Diffuse indirect light in YCoCg -> reconstruct RGB
	float  Y          = SSGI_IL_Y.Sample(SAOSampler, input.TexCoord);
	float2 CoCg       = SSGI_IL_CoCg.Sample(SAOSampler, input.TexCoord);

	float3 il_rgb;
	il_rgb.r = Y + CoCg.x - CoCg.y;
	il_rgb.g = Y + CoCg.y;
	il_rgb.b = Y - CoCg.x - CoCg.y;

	// Optional specular GI (disabled by default)
	// float3 giSpec = SSGI_Specular.Sample(SpecularSampler, input.TexCoord).rgb;
#else
	float saoCombined = saoFactor;
	float3 il_rgb     = 0.0.xxx;
#endif

	// Build pre-fog color.
	// Diffuse branch is multiplied by albedo; add IL here so it is naturally tinted by albedo.
	float4 preFog =
		((diffuse * saoCombined) + (shadowMask * dirDiffuse) + float4(il_rgb, 0.0)) * albedo
		+
		((specular * saoCombined) + (dirSpecular * shadowMask));
		// + float4(giSpec, 0.0); // enable if using specular GI

	float4 fog = FogTex.Sample(FogSampler, input.TexCoord);

	if (fog.x + fog.y + fog.z + fog.w != 0) {
		psout.Color = float4(FogNearColor.w * lerp(preFog.xyz, fog.xyz, fog.w), saturate(preFog.w));
	} else {
		psout.Color = preFog;
	}

	return psout;
}
#endif
