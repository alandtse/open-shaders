#ifndef __SKYLIGHTING_DEPENDENCY_HLSL__
#define __SKYLIGHTING_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

namespace Skylighting
{
#if defined(PSHADER)
	Texture3D<sh2> SkylightingProbeArray : register(t50);
	Texture2DArray<float3> stbn_vec3_2Dx1D_128x128x64 : register(t51);
#endif

	const static float3 ARRAY_SIZE = 4096.f * 2.5f * float3(1, 1, 0.5);

	uint3 GetArrayDims(SharedData::SkylightingSettings params)
	{
		return max(params.ArrayDims.xyz, uint3(1, 1, 1));
	}

	float3 GetCellSize(SharedData::SkylightingSettings params)
	{
		return ARRAY_SIZE / float3(GetArrayDims(params));
	}

	float getFadeOutFactor(float3 positionMS)
	{
		float3 uvw = saturate(positionMS / ARRAY_SIZE + .5);
		float3 dists = min(uvw, 1 - uvw);
		float edgeDist = min(dists.x, min(dists.y, dists.z));
		return saturate(edgeDist * 20);
	}

	float mixDiffuse(SharedData::SkylightingSettings params, float visibility)
	{
		return lerp(params.MinDiffuseVisibility, 1.0, visibility);
	}

	float mixSpecular(SharedData::SkylightingSettings params, float visibility)
	{
		return lerp(params.MinSpecularVisibility, 1.0, saturate(visibility));
	}

#if defined(PSHADER)
	void applySkylighting(inout float3 diffuseColor, inout float3 directionalAmbientColor, float3 albedo, float skylightingDiffuse)
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

		directionalAmbientColor = Color::IrradianceToGamma(Color::IrradianceToLinear(directionalAmbientColor) * Color::MultiBounceAO(Color::IrradianceToLinear(albedo / Color::PBRLightingScale), skylightingDiffuse));

		diffuseColor += directionalAmbientColor;
	}
#endif

	sh2 sample(SharedData::SkylightingSettings params, Texture3D<sh2> probeArray, Texture2DArray<float3> blueNoise, float2 screenPosition, float3 positionMS, float3 normalWS)
	{
		const uint3 arrayDims = GetArrayDims(params);
		const float3 cellSize = GetCellSize(params);
		const static sh2 unitSH = float4(sqrt(4 * Math::PI), 0, 0, 0);
		sh2 scaledUnitSH = unitSH / 1e-10;

		if (SharedData::InInterior)
			return scaledUnitSH;

		positionMS.xyz += normalWS * cellSize;  // Receiver normal bias

		if (SharedData::FrameCount) {  // Check TAA
			float3 offset = blueNoise[int3(screenPosition.xy % 128, SharedData::FrameCount % 64)] * 2.0 - 1.0;
			positionMS.xyz += offset * cellSize * 0.5;
		}

		float3 positionMSAdjusted = positionMS - params.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return scaledUnitSH;

		float3 cellVxCoord = uvw * arrayDims;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		sh2 sum = 0;
		float wsum = 0;
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 2; k++) {
					int3 offset = int3(i, j, k);
					int3 cellID = cell000 + offset;

					if (any(cellID < 0) || any((uint3)cellID >= arrayDims))
						continue;

					float3 cellCentreMS = (float3(cellID) + 0.5 - float3(arrayDims) * 0.5) * cellSize;

					float tangentWeight = 1.0;
					[branch] if (params.FastSamplingMode == 0) {
						// https://handmade.network/p/75/monter/blog/p/7288-engine_work__global_illumination_with_irradiance_probes
						// Basic tangent checks. This branch can be skipped for fast-sampling mode.
						tangentWeight = saturate(dot(normalize(cellCentreMS - positionMSAdjusted), normalWS) * 0.5 + 0.5);
					}

					float3 trilinearWeights = 1 - abs(offset - trilinearPos);
					float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z * tangentWeight;

					uint3 cellTexID = (uint3(cellID) + params.ArrayOrigin.xyz) % arrayDims;
					sh2 probe = SphericalHarmonics::Scale(probeArray[cellTexID], w);

					sum = SphericalHarmonics::Add(sum, probe);
					wsum += w;
				}

		return SphericalHarmonics::Scale(sum, rcp(wsum + 1e-10));
	}

	sh2 sampleNoBias(SharedData::SkylightingSettings params, Texture3D<sh2> probeArray, float3 positionMS)
	{
		const uint3 arrayDims = GetArrayDims(params);
		const static sh2 unitSH = float4(sqrt(4 * Math::PI), 0, 0, 0);
		sh2 scaledUnitSH = unitSH / 1e-10;

		if (SharedData::InInterior)
			return scaledUnitSH;

		float3 positionMSAdjusted = positionMS - params.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return scaledUnitSH;

		float3 cellVxCoord = uvw * arrayDims;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		sh2 sum = 0;
		float wsum = 0;
		[unroll] for (int i = 0; i < 2; i++)
			[unroll] for (int j = 0; j < 2; j++)
				[unroll] for (int k = 0; k < 2; k++)
		{
			int3 offset = int3(i, j, k);
			int3 cellID = cell000 + offset;

			if (any(cellID < 0) || any((uint3)cellID >= arrayDims))
				continue;

			float3 trilinearWeights = 1 - abs(offset - trilinearPos);
			float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z;

			uint3 cellTexID = (uint3(cellID) + params.ArrayOrigin.xyz) % arrayDims;
			sh2 probe = SphericalHarmonics::Scale(probeArray[cellTexID], w);

			sum = SphericalHarmonics::Add(sum, probe);
			wsum += w;
		}

		return SphericalHarmonics::Scale(sum, rcp(wsum + 1e-10));
	}
}

#endif
