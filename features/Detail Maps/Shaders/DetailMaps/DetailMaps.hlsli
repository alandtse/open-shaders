#ifndef __DETAIL_MAPS_HLSLI__
#define __DETAIL_MAPS_HLSLI__

namespace DetailMaps
{
#if defined(PSHADER)
	cbuffer DetailMapsPerDraw : register(b8)
	{
		float Scale;
		float AlbedoStrength;
		float NormalStrength;
		float RoughnessStrength;
		float FadeEnd;
		float FadeInvRange;
		float Transpose;
		float Pad;
	};

	Texture2D<float4> TexDetail : register(t23);

	struct Detail
	{
		float Albedo;
		float2 Slope;
		float Roughness;
	};

	Detail GetDetail(float2 uv, float depth, SamplerState samp)
	{
		Detail detail;
		detail.Albedo = 1.0;
		detail.Slope = 0.0;
		detail.Roughness = 0.0;

		float2 detailUV = (Transpose > 0.5 ? uv.yx : uv) * Scale;
		float gradScale = exp2(SharedData::MipBias);
		float2 dx = ddx(detailUV) * gradScale;
		float2 dy = ddy(detailUV) * gradScale;
		float fade = saturate((FadeEnd - depth) * FadeInvRange);

		[branch] if (fade > 0.0)
		{
			float4 texel = TexDetail.SampleGrad(samp, frac(detailUV), dx, dy);
			float2 slope = texel.ag * 2.0 - 1.0;
			detail.Albedo = lerp(1.0, texel.r * 2.0, AlbedoStrength * fade);
			detail.Slope = (Transpose > 0.5 ? slope.yx : slope) * (NormalStrength * fade);
			detail.Roughness = (texel.b - 0.5) * 2.0 * RoughnessStrength * fade;
		}
		return detail;
	}

	float3 ApplyNormal(float3 tangentNormal, Detail detail)
	{
		float3 n = normalize(tangentNormal);
		return normalize(float3(n.xy + detail.Slope * n.z, n.z));
	}
#endif
}

#endif
