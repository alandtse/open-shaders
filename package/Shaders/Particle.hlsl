#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/ReverseZ.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
cbuffer RainDrawData : register(b7)
{
	float2 RainSourceScale;
	float2 RainRasterSize;
	uint RainTestDepth;
	uint RainSceneAvailable;
	float2 RainDrawPadding;
};

float2 GetRainProjectionJitter(uint eyeIndex)
{
	float4 probe = mul(FrameBuffer::CameraProj[eyeIndex], float4(0, 0, 1, 0));
	float4 stableProbe = mul(FrameBuffer::CameraProjUnjittered[eyeIndex], float4(0, 0, 1, 0));
	return probe.xy / probe.w - stableProbe.xy / stableProbe.w;
}
#endif

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION) && defined(DYNAMIC_CUBEMAPS)
#	define REFRACTIVE_RAIN
namespace Raindrop
{
	static const float Radius = 0.003 / GAME_UNIT_TO_M;
	static const float MinimumRadiusPixels = 1.0;
}
#endif

#if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION0;
#	if !defined(ENVCUBE)
	float4 Normal: NORMAL0;
#	endif
	float4 TexCoord0: TEXCOORD0;
#	if defined(ENVCUBE)
	float4
#	else
	int4
#	endif
		TexCoord1: TEXCOORD1;
#	if defined(VR)
	uint InstanceID: SV_INSTANCEID;
#	endif  // VR
};
#endif

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float4 Color: COLOR0;
	float2 TexCoord0: TEXCOORD0;
#if defined(ENVCUBE)
	float4 PrecipitationOcclusionTexCoord: TEXCOORD1;
#endif
#if defined(VR)
	float ClipDistance: SV_ClipDistance0;  // o11
	float CullDistance: SV_CullDistance0;  // p11
	uint EyeIndex: EYEIDX0;
#endif  // VR
#if defined(REFRACTIVE_RAIN)
	float3 RaindropData: TEXCOORD2;
#endif
};

#ifdef VSHADER
cbuffer PerTechnique : register(b0)
{
	float2 ScaleAdjust : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	row_major float4x4 WorldViewProj[1];  // 0
	row_major float4x4 WorldView[1];      // 4
#	else
	row_major float4x4 WorldViewProj[2];  // 0
	row_major float4x4 WorldView[2];      // 8
#	endif
#	if defined(ENVCUBE)
	row_major float4x4 PrecipitationOcclusionWorldViewProj;  // 8, 16
#	endif
	float4 fVars0;        // 8, 16 ENVCUBE 12, 20
	float4 fVars1;        // 9, 17 ENVCUBE 13, 21
	float4 fVars2;        // 10, 18 ENVCUBE 14, 22
	float4 fVars3;        // 11, 19 ENVCUBE 15, 23
	float4 fVars4;        // 12, 20 ENVCUBE 16, 24
	float4 Color1;        // 13, 21 ENVCUBE 17, 25
	float4 Color2;        // 14, 22 ENVCUBE 18, 26
	float4 Color3;        // 15, 23 ENVCUBE 19, 27
	float4 Velocity;      // 16, 24 ENVCUBE 20, 28
	float4 Acceleration;  // 17, 25 ENVCUBE 21, 29
	float4 Wind;          // 18, 26 ENVCUBE 22, 30
}

float2x2 GetRotationMatrix(float angle)
{
	float sine, cosine;
	sincos(angle, sine, cosine);

	return float2x2(float2(cosine, -sine), float2(sine, cosine));
}

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif
	);

#	if defined(ENVCUBE)
#		if defined(RAIN)
	float2 positionOffset = input.TexCoord1.xy;
#		else
	float2x2 rotationMatrix = GetRotationMatrix(fVars0.w);
	float2 positionOffset = mul(rotationMatrix, input.TexCoord1.xy) * ScaleAdjust + mul(rotationMatrix, input.TexCoord1.zw);
#		endif

	float3 normalizedPosition = (fVars0.xyz + input.Position.xyz) / fVars2.xxx;
	normalizedPosition = normalizedPosition >= -normalizedPosition ? frac(abs(normalizedPosition)) :
	                                                                 -frac(abs(normalizedPosition));

	float4 msPosition;
	msPosition.xyz = normalizedPosition * fVars2.xxx + (-(fVars2.x * 0.5).xxx + fVars1.xyz);
	msPosition.w = 1;

#		if defined(REFRACTIVE_RAIN)
	vsout.RaindropData.z = 1.0;
	if (SharedData::rainRefractionSettings.Enable) {
		float2 corner = sign(input.TexCoord1.xy);
		float3 motion = Velocity.xyz * abs(input.TexCoord1.y) * SharedData::rainRefractionSettings.MotionStretch;
		float4 center = msPosition - float4(Velocity.xyz * 0.5, 0);
		float4 leading = mul(WorldViewProj[eyeIndex], center + float4(motion * 0.5, 0));
		float4 trailing = mul(WorldViewProj[eyeIndex], center - float4(motion * 0.5, 0));
		float4 dropPosition = lerp(trailing, leading, corner.y * 0.5 + 0.5);
		float2 renderSize = any(RainRasterSize <= 0.0) ? SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy : RainRasterSize;
#			if defined(VR)
		renderSize.x *= 0.5;
#			endif
		float2 radiusClip = Raindrop::Radius * SharedData::rainRefractionSettings.TextureRadiusScale * abs(float2(FrameBuffer::CameraProj[eyeIndex][0][0], FrameBuffer::CameraProj[eyeIndex][1][1]));
		float2 radiusPixels = radiusClip * renderSize * 0.5 / max(abs(dropPosition.w), EPSILON_DIVISION);
		float rasterScale = max(1.0, Raindrop::MinimumRadiusPixels / max(min(radiusPixels.x, radiusPixels.y), EPSILON_DIVISION));
		float2 motionPixels = (leading.xy / max(abs(leading.w), EPSILON_DIVISION) - trailing.xy / max(abs(trailing.w), EPSILON_DIVISION)) * renderSize * 0.5;
		float radius = sqrt(radiusPixels.x * radiusPixels.y);
		float rasterRadius = radius * rasterScale;
		float motionLength = length(motionPixels);
		float dropArea = Math::PI * radius * radius + 2.0 * radius * motionLength;
		float rasterArea = Math::PI * rasterRadius * rasterRadius + 2.0 * rasterRadius * motionLength;
		vsout.RaindropData.z = saturate(dropArea / max(rasterArea, EPSILON_DIVISION));
		vsout.Position = dropPosition;
		vsout.Position.xy += corner * radiusClip * rasterScale;
	} else
#		endif
	{
		float4 viewPosition = mul(WorldViewProj[eyeIndex], msPosition);
#		if defined(RAIN)
		float4 adjustedMsPosition = msPosition - float4(Velocity.xyz, 0);
		float positionBlendParam = 0.5 * (1 + input.TexCoord1.y);
		float4 adjustedViewPosition = mul(WorldViewProj[eyeIndex], adjustedMsPosition);
		float4 finalViewPosition = lerp(adjustedViewPosition, viewPosition, positionBlendParam);
#		else
		float4 finalViewPosition = viewPosition;
#		endif
		vsout.Position.xy = positionOffset + finalViewPosition.xy;
		vsout.Position.zw = finalViewPosition.zw;
	}
	vsout.Color.xyz = 1.0.xxx;
	vsout.Color.w = fVars1.w;

	vsout.TexCoord0.xy = input.TexCoord0.xy;

	float4 precipitationOcclusionTexCoord = mul(PrecipitationOcclusionWorldViewProj, msPosition);
	precipitationOcclusionTexCoord.y = -precipitationOcclusionTexCoord.y;
	vsout.PrecipitationOcclusionTexCoord = precipitationOcclusionTexCoord;
#	else
	float tmp2 = input.Normal.w * input.Position.w;
	float tmp1 = tmp2 / fVars0.y;

	float uvScale1, uvScale2, tmp3, tmp4;
	if (tmp1 > fVars2.w) {
		uvScale1 = fVars2.y;
		uvScale2 = 0;
		tmp3 = fVars2.w;
		tmp4 = 1;
	} else if (tmp1 > fVars2.z) {
		uvScale1 = fVars2.x;
		uvScale2 = fVars2.y;
		tmp3 = fVars2.z;
		tmp4 = fVars2.w;
	} else {
		uvScale1 = 0;
		uvScale2 = fVars2.x;
		tmp3 = 0;
		tmp4 = fVars2.z;
	}
	float uvScaleParam = (tmp1 - tmp3) / (tmp4 - tmp3);
	float uvScale = lerp(uvScale1, uvScale2, uvScaleParam);

	vsout.TexCoord0.xy = fVars4.xy * input.TexCoord1.xy;

	float2 uv1 = (input.TexCoord1.zw * 2.0.xx - 1.0.xx) * uvScale;
	float uvAngle = input.TexCoord0.y * input.Position.w + input.TexCoord0.x;
	float2x2 rotationMatrix = GetRotationMatrix(uvAngle);
	float2 positionOffset = mul(rotationMatrix, uv1);

	float4 msPosition;
	msPosition.xyz = -fVars3.xyz +
	                 (((input.Normal.xyz * fVars0.www + Acceleration.xyz) * (tmp2 * tmp2)) * 0.5 +
						 (((fVars0.zzz * input.Normal.xyz) * input.TexCoord0.zzz +
							  (normalize(-Wind.xyz + input.Position.xyz) * Wind.www + Velocity.xyz)) *
								 tmp2 +
							 input.Position.xyz));
	msPosition.w = 1;

	float4 viewPosition = mul(WorldViewProj[eyeIndex], msPosition);
	vsout.Position.xy = positionOffset * ScaleAdjust + viewPosition.xy;
	vsout.Position.zw = viewPosition.zw;

	float4 color1, color2;
	float colorTmp1, colorTmp2;
	if (tmp1 > fVars1.z) {
		color1 = Color3.xyzw;
		color2 = float4(Color3.xyz, 0);
		colorTmp1 = fVars1.z;
		colorTmp2 = 1;
	} else if (tmp1 > fVars1.y) {
		color1 = Color2.xyzw;
		color2 = Color3.xyzw;
		colorTmp1 = fVars1.y;
		colorTmp2 = fVars1.z;
	} else if (tmp1 > fVars1.x) {
		color1 = Color1.xyzw;
		color2 = Color2.xyzw;
		colorTmp1 = fVars1.x;
		colorTmp2 = fVars1.y;
	} else {
		color1 = float4(Color1.xyz, 0);
		color2 = Color1.xyzw;
		colorTmp1 = 0;
		colorTmp2 = fVars1.x;
	}
	float colorParam = (tmp1 - colorTmp1) / (colorTmp2 - colorTmp1);
	float4 color = lerp(color1, color2, colorParam);

	vsout.Color.w = fVars3.w * color.w;
	vsout.Color.xyz = color.xyz;
#	endif

#	if defined(REFRACTIVE_RAIN)
	// Atlas coordinates vary per streak; corner signs identify the full normal map within each drop.
	vsout.RaindropData.xy = lerp(SharedData::rainRefractionSettings.TextureBounds.xy, SharedData::rainRefractionSettings.TextureBounds.zw, step(0, input.TexCoord1.xy));
#	endif
#	if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
	if (RainTestDepth)
		vsout.Position.xy -= GetRainProjectionJitter(eyeIndex) * vsout.Position.w;
#	endif

#	ifdef VR
	vsout.EyeIndex = eyeIndex;
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.Position, eyeIndex);
	vsout.Position = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#	endif  // VR
	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
	float4 Normal: SV_Target1;
};

#ifdef PSHADER

SamplerState SampSourceTexture : register(s0);
#	if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
SamplerState SampGrayscaleTexture : register(s1);
#	endif
#	if defined(ENVCUBE)
SamplerState SampPrecipitationOcclusionTexture : register(s2);
SamplerState SampUnderwaterMask : register(s3);
#	endif

Texture2D<float4> TexSourceTexture : register(t0);
#	if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
Texture2D<float4> TexGrayscaleTexture : register(t1);
#	endif
#	if defined(ENVCUBE)
Texture2D<float4> TexPrecipitationOcclusionTexture : register(t2);
Texture2D<float4> TexUnderwaterMask : register(t3);
#	endif
#	if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
Texture2D<float4> TexRaindropNormals : register(t80);
Texture2D<float4> TexRainScene : register(t81);
Texture2D<float> TexRainDepth : register(t82);
#	endif

cbuffer PerGeometry : register(b2)
{
	float ColorScale : packoffset(c0);
	float3 TextureSize : packoffset(c1);
};

#	define LinearSampler SampSourceTexture

#	include "Common/ShadowSampling.hlsli"

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	include "Common/DirectionalShadow.hlsli"

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(DYNAMIC_CUBEMAPS)
#		define SampColorSampler SampSourceTexture
#		include "DynamicCubemaps/DynamicCubemaps.hlsli"
#	endif

#	if defined(LIGHT_LIMIT_FIX)
static const float ParticleLightDirectionEpsilon = 1e-5;
static const float ParticleLightIntensityCutoff = 1e-5;

bool GetLightCluster(float3 positionWS, uint eyeIndex, out uint lightOffset, out uint lightCount)
{
	lightOffset = 0;
	lightCount = 0;

	float3 viewPosition = FrameBuffer::WorldToView(positionWS, true, eyeIndex);
	uint clusterIndex = 0;
	if (!LightLimitFix::GetClusterIndex(FrameBuffer::ViewToUV(viewPosition, true, eyeIndex), viewPosition.z, clusterIndex))
		return false;

	lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
	lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
	return true;
}

float GetPointLightIntensity(LightLimitFix::Light light, float3 positionWS, uint eyeIndex, out float3 lightDirection)
{
	lightDirection = light.positionWS[eyeIndex].xyz - positionWS;
	float lightDist = length(lightDirection);
	lightDirection /= max(lightDist, ParticleLightDirectionEpsilon);

	float intensity = LightLimitFix::GetAttenuation(lightDist, light);
	intensity *= light.fade;
	if (intensity < ParticleLightIntensityCutoff)
		return 0.0;

	[branch] if ((light.lightFlags & LightLimitFix::LightFlags::Shadow) && light.shadowMapIndex < SharedData::lightLimitFixSettings.ShadowMapSlots)
	{
		bool shadowCoverage;
		float3 worldPositionWS = positionWS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
		intensity *= LightLimitFix::GetShadowLightShadow(light.shadowMapIndex, worldPositionWS, float2x2(1, 0, 0, 1), shadowCoverage);
	}

	return intensity;
}
#	endif

#	if defined(REFRACTIVE_RAIN)
#		include "Common/BRDF.hlsli"

namespace Raindrop
{
	static const float WaterF0 = 0.02;
	static const float Roughness = 0.3;              // Perceptual roughness of the drop's surface highlight
	static const float TransmissionExponent = 32.0;  // Tightness of the glow of a light seen through a drop
	static const float HalfVectorEpsilon = 1e-8;     // Keeps the half vector finite when the light is opposite the view
}
#	endif

/** @brief What a light shades: an unlit particle that takes light as-is, or a raindrop surface. */
struct ParticleSurface
{
	bool isRaindrop;
	float3 normal;
	float3 view;
	float3 refractDir;  ///< View ray continued through the drop
};

/** @brief Light a particle sends toward the viewer from lightColor scaled by intensity, arriving along L (toward the light). */
float3 ShadeLight(ParticleSurface surface, float3 lightColor, float intensity, float3 L)
{
#	if defined(REFRACTIVE_RAIN)
	if (surface.isRaindrop) {
		if (intensity <= 0.0)
			return 0.0;

		float3 H = (surface.view + L) * rsqrt(max(dot(surface.view + L, surface.view + L), Raindrop::HalfVectorEpsilon));
		float NdotL = saturate(dot(surface.normal, L));
		float NdotV = saturate(dot(surface.normal, surface.view));
		float NdotH = saturate(dot(surface.normal, H));
		float VdotH = saturate(dot(surface.view, H));
		float3 specular = BRDF::D_GGX(Raindrop::Roughness, NdotH) * BRDF::Vis_SmithJointApprox(Raindrop::Roughness, NdotV, NdotL) *
		                  BRDF::F_Schlick(Raindrop::WaterF0, VdotH) * NdotL;

		// A light behind the drop is seen through it along the refracted view ray; normalized Phong lobe
		float transmissionLobe = pow(saturate(dot(surface.refractDir, L)), Raindrop::TransmissionExponent) * (Raindrop::TransmissionExponent + 2.0) / Math::TAU;
		float3 transmission = transmissionLobe * (1.0 - BRDF::F_Schlick(Raindrop::WaterF0, NdotV));

		return Color::IrradianceToLinear(lightColor) * intensity * (specular + transmission);
	}
#	endif
	return lightColor * intensity;
}

/** @brief Sun color at positionWS after the world shadow; shadow is the directional and cloud shadow term still to apply. */
float3 GetDirLightColor(float3 positionWS, float2 screenPosition, uint eyeIndex, out float shadow)
{
	float screenNoise = Random::InterleavedGradientNoise(Stereo::EyeStableNoiseCoord(screenPosition, SharedData::BufferDim.xy), SharedData::FrameCount);

	float3 worldPositionWS = positionWS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

	float dirSoftShadow = 1.0;
	float dirDetailedShadow = 1.0;

	float3 dirLightColor = Color::GamutTransform(SharedData::DirLightColor.xyz);
	dirLightColor = Color::AdjustUtilityColor(dirLightColor, SharedData::csUtilitySettings.directionalLightColor);
	if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld)
		dirLightColor *= ShadowSampling::GetWorldShadow(positionWS, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

	// HasDirectionalShadows() admits Interior Sun cells to the directional shadow
	// sampling path (matching Lighting.hlsl).
	if (ShadowSampling::HasDirectionalShadows()) {
		// Use the cheaper VSM shadows if available; otherwise route through the
		// shared SLF-vs-vanilla helper (LLF cascades under SLF, else lit).
#	if defined(VOLUMETRIC_SHADOWS)
		dirSoftShadow = VolumetricShadows::GetVSMShadow2D(positionWS, worldPositionWS, eyeIndex, dirDetailedShadow);
#	else
		dirDetailedShadow = DirectionalShadow::GetSceneDirectionalShadow(positionWS, worldPositionWS, eyeIndex, screenNoise, 1.0);
#	endif
	}

	// Exactly one of dirSoftShadow / dirDetailedShadow is < 1.0 (the two paths
	// above are mutually exclusive); the other stays at its default 1.0.
	shadow = dirSoftShadow * dirDetailedShadow;
	return dirLightColor;
}

/** @brief Adds the clustered point lights at positionWS, each shaded by ShadeLight, to lighting in light order. */
void AddPointLighting(float3 positionWS, uint eyeIndex, ParticleSurface surface, float scale, inout float3 lighting)
{
#	if defined(LIGHT_LIMIT_FIX)
	uint lightOffset, lightCount;
	[branch] if (scale > 0.0 && GetLightCluster(positionWS, eyeIndex, lightOffset, lightCount))
	{
		float3 pointLighting = 0.0;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[lightOffset + i]];
			if (LightLimitFix::IsLightIgnored(light))
				continue;

			float3 lightDirection;
			float intensity = GetPointLightIntensity(light, positionWS, eyeIndex, lightDirection);
			const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
			pointLighting += ShadeLight(surface, Color::PointLight(light.color.xyz, isPointLightLinear, light.lightFlags), intensity, lightDirection);
		}
		lighting += pointLighting * scale;
	}
#	endif
}

PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout;

#	if !defined(VR)
	uint eyeIndex = 0;
#	else
	uint eyeIndex = input.EyeIndex;
#	endif  // !VR
	float2 screenPosition = input.Position.xy;
#	if defined(ENVCUBE) && defined(RAIN) && defined(RAIN_REFRACTION)
	screenPosition *= any(RainRasterSize <= 0.0) ? 1.0 : RainSourceScale;
	if (RainTestDepth) {
		float2 eyeSize = RainRasterSize * RainSourceScale;
#		if defined(VR)
		eyeSize.x *= 0.5;
#		endif
		screenPosition += GetRainProjectionJitter(eyeIndex) * float2(1, -1) * eyeSize * 0.5;
	}
	if (RainTestDepth && FrameBuffer::IsNearerDepth(TexRainDepth.Load(int3(screenPosition, 0)), input.Position.z))
		discard;
#	endif

#	if defined(ENVCUBE)
	float2 precipitationOcclusionUV = (input.PrecipitationOcclusionTexCoord.xy * 0.5 + 0.5) * TextureSize.x;
#		ifdef VR
	precipitationOcclusionUV *= FrameBuffer::DynamicResolutionParams1.x;  // only difference in VR
#		endif
	float precipitationOcclusion = -input.PrecipitationOcclusionTexCoord.z + TexPrecipitationOcclusionTexture.Load(float3(precipitationOcclusionUV, 0)).x;
	float2 underwaterMaskUv = TextureSize.yz * screenPosition;
	float underwaterMask = TexUnderwaterMask.Sample(SampUnderwaterMask, underwaterMaskUv).x;
	if (precipitationOcclusion - underwaterMask < 0) {
		discard;
	}
#	endif

#	if defined(REFRACTIVE_RAIN)
	if (SharedData::rainRefractionSettings.Enable) {
		float4 raindropNormal = TexRaindropNormals.Sample(SampSourceTexture, input.RaindropData.xy);
		float alpha = saturate(raindropNormal.w * input.RaindropData.z * (1.0 - SharedData::rainRefractionSettings.MotionTransparency));
		raindropNormal.y = 1.0 - raindropNormal.y;

		// Reconstruct camera-relative worldspace position (camera at origin).
		float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(screenPosition * SharedData::BufferDim.zw);
		uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);
		float4 posCS = float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, input.Position.z, 1.0);
		float4 posWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], posCS);
		posWS.xyz /= posWS.w;

		// Build worldspace TBN from screen-space derivatives. The billboard is camera-aligned,
		// so dPdx/dPdy lie in the billboard plane along screen X/Y.
		float3 T = normalize(ddx(posWS.xyz));
		float3 N = normalize(cross(T, -ddy(posWS.xyz)));
		float3 B = cross(N, T);
		float3x3 TBN = float3x3(T, B, N);

		float3 normalTS = normalize(raindropNormal.xyz * 2.0 - 1.0);
		float3 normalWS = normalize(mul(normalTS, TBN));
		float2 dropUV = (input.RaindropData.xy - SharedData::rainRefractionSettings.TextureBounds.xy) /
		                (SharedData::rainRefractionSettings.TextureBounds.zw - SharedData::rainRefractionSettings.TextureBounds.xy);
		float2 edgeCoverage = saturate(min(dropUV, 1.0 - dropUV) / max(fwidth(dropUV), EPSILON_DIVISION));
		alpha *= edgeCoverage.x * edgeCoverage.y;
		if (alpha <= 0.0)
			discard;

		if (frontFace)
			normalWS = -normalWS;

		float3 V = normalize(-posWS.xyz);
		float NdotV = saturate(dot(normalWS, V));
		float fresnel = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);

		float3 reflectDir = reflect(-V, normalWS);
		float3 refractDir = refract(-V, normalWS, 1.0 / 1.33);
		if (dot(refractDir, refractDir) < 1e-4)
			refractDir = -V;

		float3 reflectColor = Color::IrradianceToLinear(DynamicCubemaps::EnvReflectionsTexture.SampleLevel(SampSourceTexture, reflectDir, 0).xyz);
		float3 refractColor;
		if (RainSceneAvailable) {
			float3 positionVS = FrameBuffer::WorldToView(posWS.xyz, true, eyeIndex);
			float3 refractVS = FrameBuffer::WorldToView(refractDir, false, eyeIndex);
			float backgroundDepth = SharedData::GetScreenDepth(uv, eyeIndex);
			float rayDistance = max(backgroundDepth - positionVS.z, 0.0) / max(refractVS.z, EPSILON_DIVISION);
			float2 refractedUV = FrameBuffer::ViewToUV(positionVS + refractVS * rayDistance, true, eyeIndex);
			if (refractVS.z <= 0.0 || FrameBuffer::IsOutsideFrame(refractedUV))
				refractedUV = uv;
			refractedUV = SharedData::ConvertUVToSampleUV(refractedUV, eyeIndex);
			float depth = SharedData::DepthTexture.Load(int3(refractedUV * SharedData::BufferDim.xy, 0)).x;
			if (FrameBuffer::IsNearerDepth(depth, input.Position.z))
				refractedUV = screenPosition * SharedData::BufferDim.zw;
			float3 scene = TexRainScene.SampleLevel(SampSourceTexture, refractedUV, 0).rgb;
			refractColor = ENABLE_LL ? ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::GammaRenderTarget) ? Color::SceneGammaToLinear(scene) : scene) : Color::IrradianceToLinear(scene);
		} else {
			refractColor = Color::IrradianceToLinear(DynamicCubemaps::EnvReflectionsTexture.SampleLevel(SampSourceTexture, refractDir, 0).xyz);
		}

		// The env cubemap only carries faint bounce light, so lights are shaded on the drop directly
		ParticleSurface drop = { true, normalWS, V, refractDir };
		float dirShadow;
		float3 dirLightColor = GetDirLightColor(posWS.xyz, screenPosition, eyeIndex, dirShadow);
		float3 directLighting = ShadeLight(drop, dirLightColor, dirShadow, SharedData::DirLightDirection.xyz);
#		if defined(EFFECTS11)
		float pointLightingScale = SharedData::enbSettings.EnableParticle ? SharedData::enbSettings.ParticlePointLightingInfluence : 1.0;
#		else
		float pointLightingScale = 1.0;
#		endif
		AddPointLighting(posWS.xyz, eyeIndex, drop, pointLightingScale, directLighting);

		psout.Color.xyz = Color::IrradianceToGamma(lerp(refractColor, reflectColor, fresnel) + directLighting);
		psout.Color.w = alpha;
		psout.Normal = float4(0, 1, 0, alpha);
		if (ENABLE_LL && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::GammaRenderTarget))
			psout.Color.xyz = Color::SceneLinearToGamma(psout.Color.xyz);
		return psout;
	}
#	endif

	float4 sourceColor = TexSourceTexture.Sample(SampSourceTexture, input.TexCoord0);
	float4 baseColor = input.Color * sourceColor;
#	if defined(GRAYSCALE_TO_COLOR)
	float3 grayScaleColor =
		TexGrayscaleTexture.Sample(SampGrayscaleTexture, float2(sourceColor.y, input.Color.x)).xyz;
	baseColor.xyz = Color::TextureToWorking(grayScaleColor);
#	else
	baseColor.xyz = Color::EnbColorPow(Color::TextureToWorking(sourceColor.xyz, input.Color.xyz));
#	endif
#	if defined(GRAYSCALE_TO_ALPHA)
	float grayScaleAlpha =
		TexGrayscaleTexture.Sample(SampGrayscaleTexture, float2(sourceColor.w, input.Color.w)).w;
	baseColor.w = grayScaleAlpha;
#	endif

	float3 propertyColor = 0.0;

	float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(screenPosition * SharedData::BufferDim.zw);
	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, input.Position.z, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

	float dirShadow;
	float3 dirLightColor = GetDirLightColor(positionWS.xyz, screenPosition, eyeIndex, dirShadow);
	float3 ambientColor = Color::Ambient(max(0, SharedData::GetAmbient(float3(0, 0, 1))));
#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		ambientColor = ImageBasedLighting::GetDiffuseIBL(ambientColor, float3(0, 0, -1));
	}
#	endif

	const ParticleSurface unlit = (ParticleSurface)0;
	propertyColor += dirLightColor * dirShadow;
	propertyColor += ambientColor;
	AddPointLighting(positionWS.xyz, eyeIndex, unlit, 1.0, propertyColor);

	psout.Color.xyz = propertyColor * baseColor.xyz;
	psout.Color.w = baseColor.w;
	psout.Normal.w = baseColor.w;
	psout.Normal.xyz = float3(0, 1, 0);
	if (ENABLE_LL && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::GammaRenderTarget))
		psout.Color.xyz = Color::SceneLinearToGamma(psout.Color.xyz);

	return psout;
}
#endif
