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
		static const float RESPONSE_LAG_SECONDS = 0.45;
		/** @brief Derives leaf motion energy from raw wind speed plus independently scaled gust variation. */
		float GetLeafWindResponse(float2 baseWind, float2 sampledWind)
		{
			float flutterGain = max(Permutation::TreeLeafBaseWindFlutterGain, 0.0) *
			                    max(Permutation::TreeLeafModelSensitivity, 0.0);
			float baseSpeed = length(baseWind);
			float gustSpeed = length(sampledWind) - baseSpeed;
			float effectiveWindSpeed =
				max(baseSpeed + gustSpeed * max(Permutation::TreeLeafGustInfluence, 0.0), 0.0);
			return effectiveWindSpeed * flutterGain;
		}

		/** @brief Applies stateless inertial lag and damping to consecutive raw ambient wind targets. */
		float2 CalculateHeavyResponse(float2 currentTarget, float2 previousTarget, float frameTime)
		{
			float springStrength = max(Permutation::TreeWindSpringStrength, 0.05);
			float damping = saturate(Permutation::TreeWindSpringDamping);
			float responseLag = clamp(RESPONSE_LAG_SECONDS * rsqrt(springStrength), 0.2, 1.0);
			float lagFrameCount = frameTime > 1e-4 ? min(responseLag / frameTime, 64.0) : 0.0;
			float2 targetDelta = currentTarget - previousTarget;
			float recovering = length(currentTarget) < length(previousTarget) ? 1.0 : 0.0;
			float recoveryCarry = lerp(1.0, 0.35, damping);
			float2 response =
				currentTarget - targetDelta * lagFrameCount * lerp(1.0, recoveryCarry, recovering);

			if (dot(currentTarget, previousTarget) >= 0.0 && dot(response, currentTarget) < 0.0)
				response = 0.0.xx;

			float maximumTargetSpeed = max(length(currentTarget), length(previousTarget));
			float maximumResponseSpeed = maximumTargetSpeed * (1.0 + 0.2 * (1.0 - damping));
			float responseSpeed = length(response);
			return responseSpeed > maximumResponseSpeed && responseSpeed > 1e-5 ?
			           response * (maximumResponseSpeed / responseSpeed) :
			           response;
		}

		/** @brief Adds independently scaled sampled gust variation to the heavy trunk response. */
		float2 AddTrunkGustResponse(float2 baseWind, float2 sampledWind, float2 baseResponse)
		{
			return baseResponse +
			       (sampledWind - baseWind) * max(Permutation::TreeWindTrunkGustInfluence, 0.0);
		}

		/** @brief Converts the combined tree response into a measured-height-weighted trunk shift. */
		float2 GetTreeWorldDisplacement(float localHeight, float2 windVelocity)
		{
			float treeHeight = Permutation::TreeWindBoundsHeight;
			if (treeHeight <= 1e-3)
				return 0.0.xx;

			float normalizedHeight = saturate(
				(localHeight - Permutation::TreeWindBoundsBase) / treeHeight);
			float upperBendRange = max(Permutation::TreeWindUpperBendRange * 0.01, 0.05);
			float bendStartHeight = 1.0 - upperBendRange;
			float bendProgress = saturate((normalizedHeight - bendStartHeight) / upperBendRange);
			float flexibility = bendProgress * bendProgress;
			float maximumDisplacement =
				treeHeight * max(Permutation::TreeWindMaximumDisplacementPercent, 0.0) * 0.01;
			return windVelocity *
			       (maximumDisplacement * flexibility *
					   max(Permutation::TrunkWindBendSensitivity, 0.0) *
					   max(Permutation::TreeBendModelSensitivity, 0.0));
		}
	}

	namespace Grass
	{
		/** @brief Applies stateless lag and recovery to consecutive shared wind velocities. */
		float3 CalculateSpringVelocity(
			float3 currentVelocity, float currentGust, float3 previousVelocity, float previousGust,
			float lagFrameCount, float recoveryLagFrameCount)
		{
			float inertia = saturate(Permutation::GrassWindSpringStrength);
			float recovery = max(Permutation::GrassWindSpringRecovery, 0.0);
			float gustDelta = currentGust - previousGust;
			float gustDeviation = currentGust - 0.5;
			float recovering =
				(length(currentVelocity) < length(previousVelocity) || gustDeviation * gustDelta < 0.0) ? 1.0 : 0.0;
			float effectiveLagFrameCount = lagFrameCount + recoveryLagFrameCount * recovering;
			float3 springOffset =
				(currentVelocity - previousVelocity) * effectiveLagFrameCount *
				(inertia + recovery * recovering);
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
			float currentTargetAngle, float previousTargetAngle, float lagFrameCount,
			float recoveryLagFrameCount)
		{
			float inertia = saturate(Permutation::GrassWindSpringStrength);
			float recovery = max(Permutation::GrassWindSpringRecovery, 0.0);
			float targetDelta = currentTargetAngle - previousTargetAngle;
			float recovering = abs(currentTargetAngle) < abs(previousTargetAngle) ? 1.0 : 0.0;
			float effectiveLagFrameCount = lagFrameCount + recoveryLagFrameCount * recovering;
			float springOffset =
				targetDelta * effectiveLagFrameCount * (inertia + recovery * recovering);
			float springAngle = currentTargetAngle - springOffset;

			float maximumBendAngle = radians(max(Permutation::GrassWindMaximumTilt, 0.0));
			float maximumResponseAngle = min(
				max(abs(currentTargetAngle), abs(previousTargetAngle)) * (1.0 + recovery),
				maximumBendAngle);
			return clamp(springAngle, -maximumResponseAngle, maximumResponseAngle);
		}

		/** @brief Applies stateless lag and recovery to consecutive vertical compression targets. */
		float CalculateSpringCompression(
			float currentTargetCompression, float previousTargetCompression, float lagFrameCount,
			float recoveryLagFrameCount)
		{
			float inertia = saturate(Permutation::GrassWindSpringStrength);
			float recovery = max(Permutation::GrassWindSpringRecovery, 0.0);
			float targetDelta = currentTargetCompression - previousTargetCompression;
			float recovering = currentTargetCompression < previousTargetCompression ? 1.0 : 0.0;
			float effectiveLagFrameCount = lagFrameCount + recoveryLagFrameCount * recovering;
			float springCompression =
				currentTargetCompression -
				targetDelta * effectiveLagFrameCount * (inertia + recovery * recovering);

			float maximumResponseCompression = saturate(
				max(currentTargetCompression, previousTargetCompression) * (1.0 + recovery));
			return clamp(springCompression, 0.0, maximumResponseCompression);
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

		/** @brief Maps sampled wind velocity to desired rigid-bend and downward-compression targets. */
		void CalculateAmbientBendTarget(
			float3 worldWindVelocity, float responseScale, float4x4 worldMatrix,
			out float3 bendAxis, out float bendAngle, out float compression)
		{
			float3 modelWindVelocity = mul(transpose((float3x3)worldMatrix), worldWindVelocity);
			float3 lateralWindVelocity = float3(modelWindVelocity.xy, 0.0);
			float lateralWindSpeed = length(lateralWindVelocity);
			float downwardWindSpeed = max(-modelWindVelocity.z, 0.0);
			float3 bendDirection = lateralWindSpeed > 1e-5 ?
			                           lateralWindVelocity / lateralWindSpeed :
			                           float3(1.0, 0.0, 0.0);
			bendAxis = cross(float3(0.0, 0.0, 1.0), bendDirection);

			float maximumTilt = radians(max(Permutation::GrassWindMaximumTilt, 0.0));
			float requestedTilt =
				lateralWindSpeed * responseScale * radians(max(Permutation::GrassWindResponse, 0.0));
			bendAngle = maximumTilt > 1e-5 ? maximumTilt * tanh(requestedTilt / maximumTilt) : 0.0;
			float requestedCompression =
				downwardWindSpeed * responseScale * radians(max(Permutation::GrassWindResponse, 0.0));
			compression = maximumTilt > 1e-5 ? saturate(tanh(requestedCompression / maximumTilt)) : 0.0;
		}

		/** @brief Deforms a grass vertex from an already-resolved rigid bend. */
		float3 CalculateAmbientDisplacement(
			float tipWeight, float modelHeight, float instanceBaseHeight, float3 bendAxis,
			float rigidBendAngle, float rigidCompression, out float bendAngle)
		{
			float squaredTipWeight = saturate(tipWeight);
			squaredTipWeight *= squaredTipWeight;
			float deformationWeight = lerp(
				1.0, squaredTipWeight, saturate(Permutation::GrassWindBendProfile));
			bendAngle = rigidBendAngle * deformationWeight;
			float compression = saturate(rigidCompression * deformationWeight);

			float3 relativePosition = float3(0.0, 0.0, max(modelHeight - instanceBaseHeight, 0.0));
			float3 deformedPosition = Common::RotateVector(relativePosition, bendAxis, bendAngle);
			deformedPosition.z *= 1.0 - compression;
			return deformedPosition - relativePosition;
		}
	}
}

#endif  // __WIND_DEPENDENCY_HLSL__
