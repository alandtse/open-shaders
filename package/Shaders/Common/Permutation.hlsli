#ifndef __PERMUTATION_DEPENDENCY_HLSL__
#define __PERMUTATION_DEPENDENCY_HLSL__

namespace Permutation
{

	namespace SkyTechnique
	{
		static const uint SunOcclude = 0;
		static const uint SunGlare = 1;
		static const uint MoonAndStarsMask = 2;
		static const uint Stars = 3;
		static const uint Clouds = 4;
		static const uint CloudsLerp = 5;
		static const uint CloudsFade = 6;
		static const uint Texture = 7;
		static const uint Sky = 8;
	}

	namespace LightingTechnique
	{
		static const uint Glowmap = 2;
	}

	namespace LightingFlags
	{
		static const uint VertexColor = (1 << 0);
		static const uint Skinned = (1 << 1);
		static const uint ModelSpaceNormals = (1 << 2);
		// Flags 3 to 8 are unused
		static const uint Specular = (1 << 9);
		static const uint SoftLighting = (1 << 10);
		static const uint RimLighting = (1 << 11);
		static const uint BackLighting = (1 << 12);
		static const uint ShadowDir = (1 << 13);
		static const uint DefShadow = (1 << 14);
		static const uint ProjectedUV = (1 << 15);
		static const uint DepthWriteDecals = (1 << 15);  // (HAIR technique only)
		static const uint AnisoLighting = (1 << 16);
		static const uint AmbientSpecular = (1 << 17);
		static const uint WorldMap = (1 << 18);
		static const uint BaseObjectIsSnow = (1 << 19);
		static const uint DoAlphaTest = (1 << 20);
		static const uint Snow = (1 << 21);
		static const uint CharacterLight = (1 << 22);
		static const uint AdditionalAlphaMask = (1 << 23);
	}

	namespace WaterFlags
	{
		static const uint NormalTexCoord = (1 << 1);
		static const uint Reflections = (1 << 2);
		static const uint Refractions = (1 << 3);
		static const uint Depth = (1 << 4);
		static const uint Interior = (1 << 5);
		static const uint Wading = (1 << 6);
		static const uint VertexAlphaDepth = (1 << 7);
		static const uint Cubemap = (1 << 8);
		static const uint Flowmap = (1 << 9);
		static const uint BlendNormals = (1 << 10);
	}

	namespace EffectFlags
	{
		static const uint GrayscaleToColor = (1 << 19);
		static const uint GrayscaleToAlpha = (1 << 20);
		static const uint IgnoreTexAlpha = (1 << 21);
	}

	namespace ExtraFlags
	{
		static const uint InWorld = (1 << 0);
		static const uint InReflection = (1 << 1);
		static const uint IsBeastRace = (1 << 2);
		static const uint GrassSphereNormal = (1 << 3);
		static const uint IsSun = (1 << 4);
		static const uint SuppressExternalEmittance = (1 << 5);
		static const uint IsEye = (1 << 6);
		static const uint TreeBend = (1 << 7);
		static const uint DisableTreeAnimation = (1 << 8);
	}

	namespace ExtraFeatureFlags
	{
		static const int THLand0HasDisplacement = (1 << 0);
		static const int THLand1HasDisplacement = (1 << 1);
		static const int THLand2HasDisplacement = (1 << 2);
		static const int THLand3HasDisplacement = (1 << 3);
		static const int THLand4HasDisplacement = (1 << 4);
		static const int THLand5HasDisplacement = (1 << 5);
		static const int THLandHasDisplacement = (1 << 9);
	}

	cbuffer PerShader : register(b4)
	{
		uint VertexShaderDescriptor;
		uint PixelShaderDescriptor;
		uint ExtraShaderDescriptor;
		uint ExtraFeatureDescriptor;

		float EffectRadius;
		float TrunkWindTimer;
		float TrunkWindPreviousTimer;

		float2 TrunkWindVector;
		float2 TrunkWindPreviousVector;

		float WindIntensityOverride;
		uint OverrideWindIntensity;
		float TrunkWindGustStrength;
		float TrunkWindPreviousGustStrength;

		float TrunkWindFlexibleHeight;
		float TrunkWindMaximumDisplacement;
		float TrunkWindInstanceResponseMin;
		float TrunkWindInstanceResponseMax;

		float TrunkWindVariationMin;
		float TrunkWindVariationMax;
		float TrunkWindVariationInterval;
		float TrunkWindBendSensitivity;

		float TrunkWindLeafSensitivity;
		uint EnableGrassWindExperiment;
		float GrassWindDisplacementScale;
		float GrassWindWaveSize;

		float GrassWindWaveSpeed;
		float GrassWindWaveStrength;
		float GrassWindFlutterStrength;
		float GrassWindFlutterSpeed;

		float GrassWindVerticalBend;
		float GrassWindMaximumDisplacement;
		uint EnableGrassWindGusts;
		float GrassWindPadding;
	};

	float GetWindIntensityOverrideScale()
	{
		return OverrideWindIntensity != 0 ? WindIntensityOverride : 1.0;
	}

	float GetWindVariationSample(float sampleIndex)
	{
		return frac(sin(sampleIndex * 12.9898 + 78.233) * 43758.5453);
	}

	float GetWindVariation(float time)
	{
		float variationMin = min(TrunkWindVariationMin, TrunkWindVariationMax);
		float variationMax = max(TrunkWindVariationMin, TrunkWindVariationMax);
		float samplePosition = time / max(TrunkWindVariationInterval, 0.1);
		float sampleIndex = floor(samplePosition);
		float blend = frac(samplePosition);
		blend = blend * blend * (3.0 - 2.0 * blend);
		float variation = lerp(GetWindVariationSample(sampleIndex), GetWindVariationSample(sampleIndex + 1.0), blend);
		return lerp(variationMin, variationMax, variation);
	}

	float GetCurrentWindVariationScale()
	{
		return TrunkWindGustStrength * GetWindVariation(TrunkWindTimer);
	}

	float GetPreviousWindVariationScale()
	{
		return TrunkWindPreviousGustStrength * GetWindVariation(TrunkWindPreviousTimer);
	}

	float GetCurrentTrunkWindVariationScale()
	{
		return GetCurrentWindVariationScale() * TrunkWindBendSensitivity;
	}

	float GetPreviousTrunkWindVariationScale()
	{
		return GetPreviousWindVariationScale() * TrunkWindBendSensitivity;
	}

	float GetCurrentTrunkWindStrength()
	{
		return length(TrunkWindVector) * GetCurrentTrunkWindVariationScale();
	}

	float GetPreviousTrunkWindStrength()
	{
		return length(TrunkWindPreviousVector) * GetPreviousTrunkWindVariationScale();
	}

	float GetCurrentWindIntensityScale()
	{
		return GetWindIntensityOverrideScale() * GetCurrentWindVariationScale();
	}

	float GetPreviousWindIntensityScale()
	{
		return GetWindIntensityOverrideScale() * GetPreviousWindVariationScale();
	}

	float GetCurrentGrassWindIntensityScale()
	{
		return GetWindIntensityOverrideScale() *
		       (EnableGrassWindGusts != 0 ? GetCurrentWindVariationScale() : 1.0);
	}

	float GetPreviousGrassWindIntensityScale()
	{
		return GetWindIntensityOverrideScale() *
		       (EnableGrassWindGusts != 0 ? GetPreviousWindVariationScale() : 1.0);
	}

}
#endif  // __PERMUTATION_DEPENDENCY_HLSL__
