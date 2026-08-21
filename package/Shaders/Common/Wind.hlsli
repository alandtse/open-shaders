#ifndef __WIND_DEPENDENCY_HLSL__
#define __WIND_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"

namespace Wind
{
	namespace Common
	{
		float GetWindIntensityOverrideScale()
		{
			return Permutation::OverrideWindIntensity != 0 ? Permutation::WindIntensityOverride : 1.0;
		}

		float GetCurrentWindGustScale()
		{
			return Permutation::WindGustScale;
		}

		float GetPreviousWindGustScale()
		{
			return Permutation::WindPreviousGustScale;
		}

		float3 RotateVector(float3 inputVector, float3 axis, float angle)
		{
			float angleSin, angleCos;
			sincos(angle, angleSin, angleCos);
			return inputVector * angleCos + cross(axis, inputVector) * angleSin +
			       axis * dot(axis, inputVector) * (1.0 - angleCos);
		}
	}

	namespace Tree
	{
		float GetCurrentTreeBendScale()
		{
			return Common::GetCurrentWindGustScale() * Permutation::TrunkWindBendSensitivity;
		}

		float GetPreviousTreeBendScale()
		{
			return Common::GetPreviousWindGustScale() * Permutation::TrunkWindBendSensitivity;
		}

		float GetCurrentTreeWindStrength()
		{
			return length(Permutation::TrunkWindVector) * GetCurrentTreeBendScale();
		}

		float GetPreviousTreeWindStrength()
		{
			return length(Permutation::TrunkWindPreviousVector) * GetPreviousTreeBendScale();
		}

		float GetCurrentTreeWindIntensityScale()
		{
			return Common::GetWindIntensityOverrideScale() * Common::GetCurrentWindGustScale();
		}

		float GetPreviousTreeWindIntensityScale()
		{
			return Common::GetWindIntensityOverrideScale() * Common::GetPreviousWindGustScale();
		}

		float GetTreeWindInstanceResponse(float2 instanceOriginWS)
		{
			float2 hash = frac(instanceOriginWS * float2(0.1031, 0.1030));
			hash += dot(hash, hash.yx + 33.33);
			float responseMin = min(Permutation::TrunkWindInstanceResponseMin, Permutation::TrunkWindInstanceResponseMax);
			float responseMax = max(Permutation::TrunkWindInstanceResponseMin, Permutation::TrunkWindInstanceResponseMax);
			return lerp(responseMin, responseMax, frac((hash.x + hash.y) * hash.x));
		}

		float2 GetTreeWorldDisplacement(float localHeight, float2 windVector, float gustStrength, float instanceResponse)
		{
			float height = saturate(max(localHeight, 0.0) / max(Permutation::TrunkWindFlexibleHeight, 1.0));
			float flexibility = height * height;
			return windVector *
			       (max(Permutation::TrunkWindMaximumDisplacement, 0.0) * flexibility * gustStrength * instanceResponse);
		}
	}

	namespace Grass
	{
		float3 CalculateVanillaDisplacement(
			float2 instanceCoordinates, float tipWeight, float3 windVector, float windTimer, float windIntensityScale)
		{
			float windAngle = 0.4 * ((instanceCoordinates.x + instanceCoordinates.y) * -0.0078125 + windTimer);
			float windAngleSin, windAngleCos;
			sincos(windAngle, windAngleSin, windAngleCos);

			float windTmp3 = 0.2 * cos(Math::PI * windAngleCos);
			float windTmp1 = sin(Math::PI * windAngleSin);
			float windTmp2 = sin(Math::TAU * windAngleSin);
			float windPower = windVector.z * windIntensityScale *
			                  (((windTmp1 + windTmp2) * 0.3 + windTmp3) * (0.5 * (tipWeight * tipWeight)));

			return float3(windVector.xy, 0) * windPower;
		}

		// Two spatial scales keep neighboring clumps coherent without making the entire field repeat in lockstep.
		float3 CalculateDisplacement(
			float2 instanceCoordinates, float tipWeight, float modelHeight, float instanceBaseHeight, float3 windVector,
			float windTimer, float windIntensityScale, float2 windDirection, float gustResponse, out float bendAngle)
		{
			if (Permutation::EnableGrassWindExperiment == 0) {
				bendAngle = 0.0;
				return CalculateVanillaDisplacement(instanceCoordinates, tipWeight, windVector, windTimer, windIntensityScale);
			}

			float2 crossWindDirection = float2(-windDirection.y, windDirection.x);
			float alongWind = dot(instanceCoordinates, windDirection);
			float acrossWind = dot(instanceCoordinates, crossWindDirection);

			float coarseFrequency = Math::TAU / max(Permutation::GrassWindCoarseScale, 1.0);
			float coarseWarp = sin(
				acrossWind * coarseFrequency * 0.53 + windTimer * Permutation::GrassWindCoarseSpeed * 0.17);
			float coarsePhase = alongWind * coarseFrequency - windTimer * Permutation::GrassWindCoarseSpeed +
			                    coarseWarp * 0.65;
			float coarseSecondaryPhase =
				(alongWind * 0.61 + acrossWind * 0.79) * coarseFrequency * 0.73 -
				windTimer * Permutation::GrassWindCoarseSpeed * 0.63 + 1.7;
			float coarseNoise = sin(coarsePhase) * 0.68 + sin(coarseSecondaryPhase) * 0.32;
			float coarsePressure = smoothstep(0.15, 0.85, coarseNoise * 0.5 + 0.5);

			float fineFrequency = Math::TAU / max(Permutation::GrassWindFineScale, 1.0);
			float finePhase = alongWind * fineFrequency - windTimer * Permutation::GrassWindFineSpeed +
			                  acrossWind * fineFrequency * 0.21;
			float fineSecondaryPhase =
				(alongWind * 0.41 - acrossWind * 0.91) * fineFrequency * 1.37 -
				windTimer * Permutation::GrassWindFineSpeed * 1.31 + 2.4;
			float finePressure = sin(finePhase) * 0.6 + sin(fineSecondaryPhase) * 0.4;
			float flutterPressure = 0.5 + 0.5 * sin(
													windTimer * Permutation::GrassWindFlutterSpeed + alongWind * 0.017 + acrossWind * 0.011);

			uint2 instanceHash = Random::pcg2d(asuint(instanceCoordinates));
			float instanceResponse = lerp(0.92, 1.08, float(instanceHash.x) * (1.0 / 4294967296.0));
			float macroPressure = Permutation::EnableGrassWindGusts != 0 ? max(gustResponse, 0.0) : 1.0;
			float fieldEnergy = lerp(0.65, 1.45, saturate((macroPressure - 0.7) / 0.7));
			float spatialPressure = max(
				1.0 + fieldEnergy * (coarsePressure * Permutation::GrassWindCoarseStrength + finePressure * Permutation::GrassWindFineStrength) +
					flutterPressure * Permutation::GrassWindFlutterStrength,
				0.0);

			tipWeight = saturate(tipWeight);
			tipWeight *= tipWeight;
			float windResponse =
				windVector.z * windIntensityScale * macroPressure * spatialPressure * instanceResponse *
				Permutation::GrassWindBendScale;
			float maximumBendAngle = radians(max(Permutation::GrassWindMaximumBendAngle, 0.0));
			float bendLimit = max(maximumBendAngle, 1e-4);
			float bendRadians = windResponse * 0.007;
			float rigidBendAngle = maximumBendAngle * tanh(bendRadians / bendLimit);
			bendAngle = rigidBendAngle * lerp(1.0, tipWeight, saturate(Permutation::GrassWindCurvature));

			// A clump contains multiple blades, so each vertical vertex column must remain planted independently.
			float3 relativePosition = float3(0.0, 0.0, max(modelHeight - instanceBaseHeight, 0.0));
			float3 bendAxis = float3(-windDirection.y, windDirection.x, 0.0);
			return Common::RotateVector(relativePosition, bendAxis, bendAngle) - relativePosition;
		}

		float2 GetModelWindDirection(float2 worldWindVector, float2 vanillaWindDirection, float4x4 worldMatrix)
		{
			float useSharedDirection = dot(worldWindVector, worldWindVector) > 1e-6;
			float2 sourceDirection = lerp(vanillaWindDirection, worldWindVector, useSharedDirection);
			float3 modelDirection = mul(transpose((float3x3)worldMatrix), float3(sourceDirection, 0.0));
			return modelDirection.xy * rsqrt(max(dot(modelDirection.xy, modelDirection.xy), 1e-6));
		}
	}
}

#endif  // __WIND_DEPENDENCY_HLSL__
