
#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/VR.hlsli"
#include "Common/WindField.hlsli"

Texture2D<float3> SpecularTexture : register(t0);
Texture2D<unorm float3> AlbedoTexture : register(t1);
Texture2D<unorm float3> NormalRoughnessTexture : register(t2);
Texture2D<float3> MasksTexture : register(t3);
Texture2D<unorm float2> Masks2Texture : register(t9);
Texture2D<float4> GrassWindSpringDebug : register(t27);

RWTexture2D<float4> MainRW : register(u0);
RWTexture2D<float4> NormalTAAMaskSpecularMaskRW : register(u1);
RWTexture2D<float2> MotionVectorsRW : register(u2);

// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT (no unorm),
// OFF -> R24_UNORM_X8_TYPELESS game depth (unorm).
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t4);
#else
Texture2D<SCENE_DEPTH_FORMAT> DepthTexture : register(t4);
#endif

#if defined(DYNAMIC_CUBEMAPS)
Texture2D<float3> ReflectanceTexture : register(t5);
TextureCube<float3> EnvTexture : register(t6);
TextureCube<float3> EnvReflectionsTexture : register(t7);

SamplerState LinearSampler : register(s0);
#endif

#if defined(SKYLIGHTING)
#	define SKYLIGHTING_PROBE_REGISTER t8
#	define SKYLIGHTING_COVERAGE_REGISTER t18
#	define SKYLIGHTING_ZENITH_REGISTER t19
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(SSGI) || defined(SSR)
#include "NRD/NRDReblurSH.hlsli"
#endif
#if defined(SSGI)
Texture2D<float4> SsgiTexture : register(t10);
#if defined(VR)
Texture2D<float4> SsgiRightTexture : register(t11);
#endif


bool IsSkinMask(float2 mask) { return mask.x > 0.0 && mask.y < 1.5; }
float SampleSSGIAO(uint2 pixCoord) {
#if defined(VR)
    uint w = uint(SharedData::BufferDim.x * FrameBuffer::DynamicResolutionParams1.x * 0.5);
    if (pixCoord.x >= w) return saturate(SsgiRightTexture[pixCoord-uint2(w,0)].w);
#endif
    return saturate(SsgiTexture[pixCoord].w);
}
float SampleSSGIAOSkin(uint2 pixCoord)
{
	int2 maxCoord = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy) - 1;
    int2 minCoord = 0;
#if defined(VR)
    int w = (maxCoord.x+1)/2;
    minCoord.x = pixCoord.x >= w ? w : 0;
    maxCoord.x = minCoord.x+w-1;
#endif
	const float centerDepth = SharedData::GetScreenDepth(DepthTexture[pixCoord]);
	float ao = 0.0;
	float totalWeight = 0.0;
	[unroll] for (int y = -3; y <= 3; y += 3)
	{
		[unroll] for (int x = -3; x <= 3; x += 3)
		{
			const int2 tap = clamp(int2(pixCoord) + int2(x, y), minCoord, maxCoord);
			const float difference = abs(SharedData::GetScreenDepth(DepthTexture[tap]) - centerDepth) / max(centerDepth * 0.01, 1.0);
			const float weight = exp2(-difference * difference) * (IsSkinMask(MasksTexture[tap].xy) ? 1.0 : 0.05);
			ao += SampleSSGIAO(tap) * weight;
			totalWeight += weight;
		}
	}
	return ao / max(totalWeight, EPSILON_DIVISION);
}
#endif

#if defined(SSR)
Texture2D<float4> SsrTexture : register(t13);
#if defined(VR)
Texture2D<float4> SsrRightTexture : register(t12);
#endif
void SampleSSRTracedSpecular(uint2 pixel, out float3 radiance, out float hit) {
    #if defined(VR)
    uint w=uint(SharedData::BufferDim.x*FrameBuffer::DynamicResolutionParams1.x*0.5);
    if(pixel.x>=w) { REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SsrRightTexture[pixel-uint2(w,0)],radiance,hit); return; }
#endif
    REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SsrTexture[pixel], radiance, hit);
}
#endif

#if defined(IBL)
#	if !defined(DYNAMIC_CUBEMAPS)
#		undef IBL
#	else
#		define IBL_DEFERRED
#		include "IBL/IBL.hlsli"
#	endif
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float3 normalGlossiness = NormalRoughnessTexture[dispatchID.xy];
	float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);

	float3 diffuseColor = MainRW[dispatchID.xy].xyz;
	if (ENABLE_LL)
		diffuseColor = Color::SceneGammaToLinear(diffuseColor);
	float3 specularColor = SpecularTexture[dispatchID.xy];
	float3 albedo = AlbedoTexture[dispatchID.xy];

	float depth = DepthTexture[dispatchID.xy];
	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

	if (depth == FrameBuffer::FarPlaneDepth())
		MotionVectorsRW[dispatchID.xy] = MotionBlur::GetSSMotionVector(positionWS, positionWS, eyeIndex);  // Apply sky motion vectors

	float glossiness = normalGlossiness.z;

	float3 linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);

#if defined(SKYLIGHTING)
#	if defined(VR)
	float3 positionMS = positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#	else
	float3 positionMS = positionWS.xyz;
#	endif
	// Normal-biased, like forward Lighting.hlsl: a reflection-vector bias slides along grazing surfaces and shows the probe grid.
	sh2 skylightingSH = Skylighting::Sample(positionMS, normalWS);
#endif

float ssgiAo = 1.0;
#if defined(SSGI)

	if ((SharedData::ssgiSettings.Enabled & (1u << eyeIndex)) && abs(depth - FrameBuffer::FarPlaneDepth()) > 1e-6) {
	ssgiAo = IsSkinMask(MasksTexture[dispatchID.xy].xy) ? SampleSSGIAOSkin(dispatchID.xy) : SampleSSGIAO(dispatchID.xy);
	if ((SharedData::ssgiSettings.ReplaceAmbient & (1u << eyeIndex)) == 0) ssgiAo = pow(max(ssgiAo, EPSILON_DIVISION), SharedData::ssgiSettings.AOPower);

	// Masks2.x stores 1 - vertexAO (Lighting.hlsl only); cleared to 0 for
	// pixels with no vertex AO contribution, so vertexAO defaults to 1.
	float vertexAO = 1.0 - Masks2Texture[dispatchID.xy].x;
	ssgiAo = saturate(ssgiAo / max(vertexAO, EPSILON_DIVISION));

	float3 linAlbedo = Color::IrradianceToLinear(albedo / Color::PBRLightingScale);
	float3 multiBounceSSGIAo = MultiBounceAO(linAlbedo, ssgiAo);

	if ((SharedData::ssgiSettings.ReplaceAmbient & (1u << eyeIndex)) != 0) {
		linDiffuseColor *= sqrt(multiBounceSSGIAo);
	} else {
	float3 vanillaDALC = Color::Ambient(max(0, SharedData::GetAmbient(normalWS)));
	float3 directionalAmbientColor = vanillaDALC;

#	if defined(IBL)
	[branch] if (SharedData::iblSettings.EnableIBL)
	{
#		if defined(SKYLIGHTING)
		float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, normalWS, Skylighting::GetFadeOutFactor(positionMS));
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBLOccluded(vanillaDALC, -normalWS, skylightingDiffuse);
#		else
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(vanillaDALC, -normalWS);
#		endif
	}
#	endif

	directionalAmbientColor *= albedo;
	directionalAmbientColor = Color::RGBToYCoCg(directionalAmbientColor);
	directionalAmbientColor.x = MasksTexture[dispatchID.xy].z;
	directionalAmbientColor = Color::YCoCgToRGB(directionalAmbientColor);
	directionalAmbientColor = max(0, directionalAmbientColor);

	{
		float maxScale = 1.0;
		if (directionalAmbientColor.x > 0.0)
			maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
		if (directionalAmbientColor.y > 0.0)
			maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
		if (directionalAmbientColor.z > 0.0)
			maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
		directionalAmbientColor *= maxScale;

		diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);
		linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
		linDiffuseColor *= sqrt(multiBounceSSGIAo);
		diffuseColor = Color::IrradianceToGamma(linDiffuseColor);
		diffuseColor += Color::IrradianceToGamma(Color::IrradianceToLinear(directionalAmbientColor) * multiBounceSSGIAo);
		linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
	}

	}
	}
#endif

	float3 color = linDiffuseColor + specularColor;

#if defined(DYNAMIC_CUBEMAPS)

	float3 reflectance = ReflectanceTexture[dispatchID.xy];

	if (any(reflectance > 0.0)) {
		float3 V = -normalize(positionWS.xyz);
		float3 R = reflect(-V, normalWS);

		float roughness = 1.0 - glossiness;
		float level = roughness * 8.0;

		sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normalWS, V, roughness);

		float3 finalIrradiance = 0;

		float directionalAmbientColorSpecular = Color::RGBToLuminance(Color::Ambient(max(0, SharedData::GetAmbient(R)))) * Color::ReflectionNormalisationScale;
#	if defined(IBL) || defined(SKYLIGHTING)
		float skylightingSpecular = 1.0;
		float skylightingVisibility = 1.0;
#	endif

#	if defined(SKYLIGHTING)
		sh2 skylightingSpecularSH = Skylighting::SampleSpecular(skylightingSH, positionMS);
		float skylightingFadeOut = Skylighting::GetHorizontalFadeOutFactor(positionMS);
		skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSpecularSH, specularLobe, skylightingFadeOut);
		skylightingVisibility = Skylighting::EvaluateVisibility(skylightingSpecularSH, skylightingFadeOut);
#	endif

#	if defined(IBL)
		[branch] if (SharedData::iblSettings.EnableIBL)
		{
			float3 envSpecular;
			float3 skySpecular;
			ImageBasedLighting::ComputeSpecularIBL(
				EnvTexture,
				EnvReflectionsTexture,
				LinearSampler,
				R,
				level,
				directionalAmbientColorSpecular,
				skylightingSpecular,
				skylightingVisibility,
				envSpecular,
				skySpecular);

			finalIrradiance = envSpecular + skySpecular;
		}
		else
#	endif
		{
			// Fallback without IBL: normalize-by-luminance with DALC
#	if defined(INTERIOR)
			float3 specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);
			float specularIrradianceLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));
			specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;
			finalIrradiance = Color::IrradianceToLinear(specularIrradiance);
#	elif defined(SKYLIGHTING)
			float3 specularIrradianceReflections = 0.0;
			if (skylightingSpecular > 0.0) {
				specularIrradianceReflections = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);
				float lum = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));
				specularIrradianceReflections = (specularIrradianceReflections / max(lum, 0.001)) * directionalAmbientColorSpecular;
				specularIrradianceReflections = Color::IrradianceToLinear(specularIrradianceReflections);
			}
			float3 specularIrradiance = 0.0;
			if (skylightingSpecular < 1.0) {
				specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);
				float lum = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));
				float dalcScaled = Color::IrradianceToGamma(Color::IrradianceToLinear(directionalAmbientColorSpecular) * skylightingSpecular);
				specularIrradiance = (specularIrradiance / max(lum, 0.001)) * dalcScaled;
				specularIrradiance = Color::IrradianceToLinear(specularIrradiance);
			}
			finalIrradiance = lerp(specularIrradiance, specularIrradianceReflections, skylightingSpecular);
#	else
			float3 specularIrradiance = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);
			float specularIrradianceLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));
			specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;
			finalIrradiance = Color::IrradianceToLinear(specularIrradiance);
#	endif
		}

# if defined(SSR)
        if (SharedData::ssrSettings.Enabled & (1u << eyeIndex)) {
            float3 traced; float hit;
            SampleSSRTracedSpecular(dispatchID.xy, traced, hit);
            finalIrradiance = lerp(finalIrradiance * SharedData::ssrSettings.SpecCubemapMult,
                traced, saturate(hit / NRD_EPS));
        }
# endif
# if defined(SSGI)
        if (SharedData::ssgiSettings.Enabled & (1u << eyeIndex))
            finalIrradiance *= SpecularOcclusion(saturate(dot(normalWS,V)), roughness * roughness, ssgiAo);
# endif

		color += reflectance * finalIrradiance;
	}

#endif

	color = Color::SceneLinearToGamma(color);

#if defined(DEBUG)

#	if defined(VR)
	uv.x += (eyeIndex ? 0.1 : -0.1);
#	endif  // VR

	if (uv.x < 0.5 && uv.y < 0.5) {
		color = color;
	} else if (uv.x < 0.5) {
		color = albedo;
	} else if (uv.y < 0.5) {
		color = normalVS;
	} else {
		color = glossiness;
	}

#endif

	[branch] if (SharedData::windSettings.windFieldDebugEnabled != 0u && depth < 1.0f)
	{
		float3 worldPosition = positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
		uint debugView = SharedData::windSettings.windFieldDebugView;
		if (debugView == 5u) {
			color = 0.0f;
			if (SharedData::WindFieldSpringDebug.z > 0.0f) {
				float2 springUV = (worldPosition.xy - SharedData::WindFieldSpringDebug.xy) /
				                  SharedData::WindFieldSpringDebug.z;
				if (all(springUV >= 0.0f) && all(springUV <= 1.0f)) {
					uint springWidth, springHeight;
					GrassWindSpringDebug.GetDimensions(springWidth, springHeight);
					uint2 springCell = min(uint2(springUV * float2(springWidth, springHeight)),
						uint2(springWidth - 1u, springHeight - 1u));
					float3 springResponse = GrassWindSpringDebug.Load(int3(springCell, 0)).xyz;
					float springMagnitude = saturate(length(springResponse.xy) /
													 max(SharedData::WindFieldSpringDebug.w, EPSILON_WIND_GEOMETRY));
					color = lerp(Color::TurboColormap(springMagnitude), float3(1.0f, 1.0f, 1.0f), springResponse.z * 0.25f);
				}
			}
		} else if (debugView == 6u) {
			WindField::TransientImpulseSample impulseSample = WindField::SampleCurrentTransientImpulses(worldPosition);
			float intensity = saturate(impulseSample.intensity);
			color = Color::TurboColormap(intensity) * intensity;
		} else {
			WindField::WindSample currentSample = WindField::SampleField(
				worldPosition, SharedData::WindFieldCurrent, SharedData::WindFieldTuning);
			WindField::WindSample previousSample = currentSample;
			if (debugView >= 2u && SharedData::WindFieldTransitionData.x < 1.0f)
				previousSample = WindField::SampleField(
					worldPosition, SharedData::WindFieldTransition, SharedData::WindFieldTuning);
			WindField::WindSample displayedSample = WindField::SampleCurrent(worldPosition, 1.0f, 1.0f);
			if (debugView == 1u)
				displayedSample = currentSample;
			else if (debugView == 2u)
				displayedSample = previousSample;
			else if (debugView == 3u) {
				if (((dispatchID.x / 16u + dispatchID.y / 16u) & 1u) != 0u)
					displayedSample = previousSample;
			} else if (debugView == 4u) {
				if (dispatchID.x * 2u < uint(SharedData::BufferDim.x))
					displayedSample = previousSample;
			}
			float ambientPressure = saturate((displayedSample.ambientGust - 0.5f) * 2.0f);
			color = Color::TurboColormap(max(ambientPressure, saturate(displayedSample.transientImpulse)));
		}
	}

	MainRW[dispatchID.xy] = float4(color, 1.0);
	NormalTAAMaskSpecularMaskRW[dispatchID.xy] = float4(GBuffer::EncodeNormalVanilla(normalVS), 0.0, 0.0);
}
