// SPDX-License-Identifier: LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception
#ifndef __DETAIL_MAPS_HLSLI__
#define __DETAIL_MAPS_HLSLI__

namespace DetailMaps
{
#if defined(PSHADER)
	Texture2D<float4> TexDetail : register(t23);

	struct Detail
	{
		float Albedo;
		float2 Slope;
		float Roughness;
	};

	Detail GetDetail(float2 uv, float depth, SamplerState samp)
	{
		const float scale = Permutation::PerDrawFeatureData[0].x;
		const float albedoStrength = Permutation::PerDrawFeatureData[0].y;
		const float normalStrength = Permutation::PerDrawFeatureData[0].z;
		const float roughnessStrength = Permutation::PerDrawFeatureData[0].w;
		const float fadeEnd = Permutation::PerDrawFeatureData[1].x;
		const float fadeInvRange = Permutation::PerDrawFeatureData[1].y;
		const float transpose = Permutation::PerDrawFeatureData[1].z;

		Detail detail;
		detail.Albedo = 1.0;
		detail.Slope = 0.0;
		detail.Roughness = 0.0;

		float2 detailUV = (transpose > 0.5 ? uv.yx : uv) * scale;
		float gradScale = exp2(SharedData::MipBias);
		float2 dx = ddx(detailUV) * gradScale;
		float2 dy = ddy(detailUV) * gradScale;
		float fade = saturate((fadeEnd - depth) * fadeInvRange);

		[branch] if (fade > 0.0)
		{
			float4 texel = TexDetail.SampleGrad(samp, frac(detailUV), dx, dy);
			float2 slope = texel.ag * 2.0 - 1.0;
			detail.Albedo = max(0.0, lerp(1.0, texel.r * 2.0, albedoStrength * fade));
			detail.Slope = (transpose > 0.5 ? slope.yx : slope) * (normalStrength * fade);
			detail.Roughness = (texel.b - 0.5) * 2.0 * roughnessStrength * fade;
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
