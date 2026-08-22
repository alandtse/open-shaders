#ifndef __WIND_DEPENDENCY_HLSL__
#define __WIND_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"

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
		/** @brief Applies stateless lag and recovery to consecutive ambient velocities. */
		float3 CalculateSpringVelocity(
			float3 currentVelocity, float currentGust, float3 previousVelocity, float previousGust,
			float lagFrameCount)
		{
			float inertia = saturate(Permutation::GrassWindSpringStrength);
			float recovery = max(Permutation::GrassWindSpringRecovery, 0.0);
			float gustDelta = currentGust - previousGust;
			float gustDeviation = currentGust - 0.5;
			float recovering = gustDeviation * gustDelta < 0.0 ? 1.0 : 0.0;
			float3 springOffset =
				(currentVelocity - previousVelocity) * lagFrameCount * (inertia + recovery * recovering);
			float3 springVelocity = currentVelocity - springOffset;

			float maximumResponseSpeed =
				max(length(currentVelocity), length(previousVelocity)) * (1.0 + recovery);
			float springSpeed = length(springVelocity);
			return springSpeed > maximumResponseSpeed && springSpeed > 1e-5 ?
			           springVelocity * (maximumResponseSpeed / springSpeed) :
			           springVelocity;
		}

		/** @brief Applies stateless lag and recovery to consecutive rigid-bend targets. */
		float CalculateSpringAngle(
			float currentTargetAngle, float previousTargetAngle, float lagFrameCount)
		{
			float inertia = saturate(Permutation::GrassWindSpringStrength);
			float recovery = max(Permutation::GrassWindSpringRecovery, 0.0);
			float targetDelta = currentTargetAngle - previousTargetAngle;
			float recovering = targetDelta < 0.0 ? 1.0 : 0.0;
			float springOffset = targetDelta * lagFrameCount * (inertia + recovery * recovering);
			float springAngle = currentTargetAngle - springOffset;

			float maximumBendAngle = radians(max(Permutation::GrassWindMaximumTilt, 0.0));
			float maximumResponseAngle = min(
				max(abs(currentTargetAngle), abs(previousTargetAngle)) * (1.0 + recovery),
				maximumBendAngle);
			return clamp(springAngle, -maximumResponseAngle, maximumResponseAngle);
		}

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

		/** @brief Maps sampled ambient velocity to a desired rigid-bend target. */
		void CalculateAmbientBendTarget(
			float3 worldWindVelocity, float responseScale, float4x4 worldMatrix,
			out float3 bendAxis, out float bendAngle)
		{
			float3 modelWindVelocity = mul(transpose((float3x3)worldMatrix), worldWindVelocity);
			float3 lateralWindVelocity = float3(modelWindVelocity.xy, 0.0);
			float lateralWindSpeed = length(lateralWindVelocity);
			float3 bendDirection = lateralWindSpeed > 1e-5 ?
			                           lateralWindVelocity / lateralWindSpeed :
			                           float3(1.0, 0.0, 0.0);
			bendAxis = cross(float3(0.0, 0.0, 1.0), bendDirection);

			float maximumTilt = radians(max(Permutation::GrassWindMaximumTilt, 0.0));
			float requestedTilt =
				lateralWindSpeed * responseScale * radians(max(Permutation::GrassWindResponse, 0.0));
			bendAngle = maximumTilt > 1e-5 ? maximumTilt * tanh(requestedTilt / maximumTilt) : 0.0;
		}

		/** @brief Deforms a grass vertex from an already-resolved rigid bend. */
		float3 CalculateAmbientDisplacement(
			float tipWeight, float modelHeight, float instanceBaseHeight, float3 bendAxis,
			float rigidBendAngle, out float bendAngle)
		{
			float squaredTipWeight = saturate(tipWeight);
			squaredTipWeight *= squaredTipWeight;
			bendAngle = rigidBendAngle * lerp(
											 1.0, squaredTipWeight, saturate(Permutation::GrassWindBendProfile));

			float3 relativePosition = float3(0.0, 0.0, max(modelHeight - instanceBaseHeight, 0.0));
			return Common::RotateVector(relativePosition, bendAxis, bendAngle) - relativePosition;
		}
	}
}

#endif  // __WIND_DEPENDENCY_HLSL__
