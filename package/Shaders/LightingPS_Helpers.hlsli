float GetSoftLightMultiplier(float angle)
{
	float softLightParam = saturate((LightingEffectParams.x + angle) / (1 + LightingEffectParams.x));
	float arg1 = (softLightParam * softLightParam) * (3 - 2 * softLightParam);
	float clampedAngle = saturate(angle);
	float arg2 = (clampedAngle * clampedAngle) * (3 - 2 * clampedAngle);
	float softLigtMul = saturate(arg1 - arg2);
	return softLigtMul;
}

float GetRimLightMultiplier(float3 L, float3 V, float3 N)
{
	float NdotV = saturate(dot(N, V));
	return exp2(LightingEffectParams.y * log2(1 - NdotV)) * saturate(dot(V, -L));
}

#if !defined(TRUE_PBR)
float ProcessSparkleColor(float color)
{
	return exp2(SparkleParams.y * log2(min(1, abs(color))));
}
#endif

float3 TransformNormal(float3 normal)
{
	return normal * 2 + -1.0.xxx;
}

float GetLodLandBlendParameter(float3 color)
{
	float result = saturate(1.6666666 * (dot(color, 0.55.xxx) - 0.4));
	result = ((result * result) * (3 - result * 2));
#if !defined(WORLD_MAP)
	result *= 0.8;
#endif
	return result;
}

float GetLodLandBlendMultiplier(float parameter, float mask)
{
	return 0.8333333 * (parameter * (0.37 - mask) + mask) + 0.37;
}

float GetLandSnowMaskValue(float alpha)
{
#if !defined(TRUE_PBR)
	return alpha * LandscapeTexture5to6IsSnow.z + (1 + -LandscapeTexture5to6IsSnow.z);
#else
	return 0;
#endif
}

float3 GetLandNormal(float landSnowMask, float3 normal, float2 uv, SamplerState sampNormal, Texture2D<float4> texNormal)
{
	float3 landNormal = TransformNormal(normal);
#if defined(SNOW) && !defined(TRUE_PBR)
	if (landSnowMask > 1e-5 && LandscapeTexture5to6IsSnow.w != 1.0) {
		float3 snowNormal =
			float3(-1, -1, 1) *
			TransformNormal(texNormal.Sample(sampNormal, LandscapeTexture5to6IsSnow.ww * uv).xyz);
		landNormal.z += 1;
		float normalProjection = dot(landNormal, snowNormal);
		snowNormal = landNormal * normalProjection.xxx - snowNormal * landNormal.z;
		return normalize(snowNormal);
	} else {
		return landNormal;
	}
#else
	return landNormal;
#endif
}

#if defined(SNOW) && !defined(TRUE_PBR)
float3 GetSnowSpecularColor(PS_INPUT input, float3 worldNormal, float3 viewDirection)
{
	if (SnowRimLightParameters.w > 1e-5) {
#	if defined(MODELSPACENORMALS) && !defined(SKINNED)
		float3 modelGeometryNormal = float3(0, 0, 1);
#	else
		float3 modelGeometryNormal = normalize(float3(input.TBN0.z, input.TBN1.z, input.TBN2.z));
#	endif
		float normalFactor = 1 - saturate(dot(worldNormal, viewDirection));
		float geometryNormalFactor = 1 - saturate(dot(modelGeometryNormal, viewDirection));
		return (SnowRimLightParameters.x * (exp2(SnowRimLightParameters.y * log2(geometryNormalFactor)) * exp2(SnowRimLightParameters.z * log2(normalFactor)))).xxx;
	} else {
		return 0.0.xxx;
	}
}
#endif

#if defined(FACEGEN)
float3 GetFacegenBaseColor(float3 rawBaseColor, float2 uv)
{
	float3 detailColor = TexDetailSampler.Sample(SampDetailSampler, uv).xyz;
	detailColor = float3(3.984375, 3.984375, 3.984375) * (float3(0.00392156886, 0, 0.00392156886) + detailColor);
	float3 tintColor = TexTintSampler.Sample(SampTintSampler, uv).xyz;
	tintColor = tintColor * rawBaseColor * 2.0.xxx;
	tintColor = tintColor - tintColor * rawBaseColor;
	return (rawBaseColor * rawBaseColor + tintColor) * detailColor;
}
#endif

#if defined(FACEGEN_RGB_TINT)
float3 GetFacegenRGBTintBaseColor(float3 rawBaseColor, float2 uv)
{
	float3 tintColor = TintColor.xyz * rawBaseColor * 2.0.xxx;
	tintColor = tintColor - tintColor * rawBaseColor;
	return float3(1.01171875, 0.99609375, 1.01171875) * (rawBaseColor * rawBaseColor + tintColor);
}
#endif

#if defined(WORLD_MAP)
float3 GetWorldMapNormal(PS_INPUT input, float3 rawNormal, float3 baseColor)
{
	float3 normal = normalize(rawNormal);
#	if defined(MODELSPACENORMALS)
	float3 worldMapNormalSrc = normal.xyz;
#	else
	float3 worldMapNormalSrc = float3(input.TBN0.z, input.TBN1.z, input.TBN2.z);
#	endif
	float3 worldMapNormal = 7.0.xxx * (-0.2.xxx + abs(normalize(worldMapNormalSrc)));
	worldMapNormal = max(0.01.xxx, worldMapNormal * worldMapNormal * worldMapNormal);
	worldMapNormal /= dot(worldMapNormal, 1.0.xxx);
	float3 worldMapColor1 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.yz).xyz;
	float3 worldMapColor2 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.xz).xyz;
	float3 worldMapColor3 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.xy).xyz;
#	if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float3 worldMapSnowColor1 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.yz).xyz;
	float3 worldMapSnowColor2 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.xz).xyz;
	float3 worldMapSnowColor3 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.xy).xyz;
#	endif
	float3 worldMapColor = worldMapNormal.xxx * worldMapColor1 + worldMapNormal.yyy * worldMapColor2 + worldMapNormal.zzz * worldMapColor3;
#	if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float3 worldMapSnowColor = worldMapSnowColor1 * worldMapNormal.xxx + worldMapSnowColor2 * worldMapNormal.yyy + worldMapSnowColor3 * worldMapNormal.zzz;
	float snowMultiplier = GetLodLandBlendParameter(baseColor);
	worldMapColor = snowMultiplier * (worldMapSnowColor - worldMapColor) + worldMapColor;
#	endif
	worldMapColor = normalize(2.0.xxx * (-0.5.xxx + (worldMapColor)));
#	if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float worldMapLandTmp = saturate(19.9999962 * (rawNormal.z - 0.95));
	worldMapLandTmp = saturate(-(worldMapLandTmp * worldMapLandTmp) * (worldMapLandTmp * -2 + 3) + 1.5);
	float3 worldMapLandTmp1 = normalize(normal.zxy * float3(1, 0, 0) - normal.yzx * float3(0, 0, 1));
	float3 worldMapLandTmp2 = normalize(worldMapLandTmp1.yzx * normal.zxy - worldMapLandTmp1.zxy * normal.yzx);
	float3 worldMapLandTmp3 = normalize(worldMapColor.xxx * worldMapLandTmp1 + worldMapColor.yyy * worldMapLandTmp2 + worldMapColor.zzz * normal.xyz);
	float worldMapLandTmp4 = dot(worldMapLandTmp3, worldMapLandTmp3);
	if (worldMapLandTmp4 > 0.999 && worldMapLandTmp4 < 1.001) {
		normal.xyz = worldMapLandTmp * (worldMapLandTmp3 - normal.xyz) + normal.xyz;
	}
#	else
	normal.xyz = normalize(
		WorldMapOverlayParametersPS.zzz * (rawNormal.xyz - worldMapColor.xyz) + worldMapColor.xyz);
#	endif
	return normal;
}

float3 GetWorldMapBaseColor(float3 originalBaseColor, float3 rawBaseColor, float texProjTmp)
{
#	if defined(LODOBJECTS) && !defined(PROJECTED_UV)
	return rawBaseColor;
#	endif
#	if defined(LODLANDSCAPE) || defined(LODOBJECTSHD) || defined(LODLANDNOISE)
	float lodMultiplier = GetLodLandBlendParameter(originalBaseColor.xyz);
#	elif defined(LODOBJECTS) && defined(PROJECTED_UV)
	float lodMultiplier = saturate(10 * texProjTmp);
#	else
	float lodMultiplier = 1;
#	endif
#	if defined(LODOBJECTS)
	float4 lodColorMul = lodMultiplier.xxxx * float4(0.269999981, 0.281000018, 0.441000015, 0.441000015) + float4(0.0780000091, 0.09799999, -0.0349999964, 0.465000004);
	float4 lodColor = lodColorMul.xyzw * 2.0.xxxx;
	lodColor.xyz = Color::Diffuse(lodColor.xyz);
	bool useLodColorZ = lodColorMul.w > 0.5;
	lodColor.xyz = max(lodColor.xyz, rawBaseColor.xyz);
	lodColor.w = useLodColorZ ? lodColor.z : min(lodColor.w, rawBaseColor.z);
	return (0.5 * lodMultiplier).xxx * (lodColor.xyw - rawBaseColor.xyz) + rawBaseColor;
#	else
	float4 lodColorMul = lodMultiplier.xxxx * float4(0.199999988, 0.441000015, 0.269999981, 0.281000018) + float4(0.300000012, 0.465000004, 0.0780000091, 0.09799999);
	float3 lodColor = lodColorMul.zwy * 2.0.xxx;
	lodColor.xyz = Color::Diffuse(lodColor.xyz);
	lodColor.xy = max(lodColor.xy, rawBaseColor.xy);
	lodColor.z = lodColorMul.y > 0.5 ? max((lodMultiplier * 0.441 + -0.0349999964) * 2, rawBaseColor.z) : min(lodColor.z, rawBaseColor.z);
	return lodColorMul.xxx * (lodColor - rawBaseColor.xyz) + rawBaseColor;
#	endif
}
#endif

float GetSnowParameterY(float texProjTmp, float alpha)
{
	if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::BaseObjectIsSnow) {
		return min(1, texProjTmp + alpha);
	}
	return texProjTmp;
}
