#ifndef __GRASS_WIND_DEPENDENCY_HLSL__
#define __GRASS_WIND_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"

namespace GrassWind
{
	static const float FlutterResponseRadians = 1.5f;
	static const float FlutterMaximumAngleRadians = Math::PI / 18.0f;
	static const float FlutterPhaseScale = 0.4f;

	float CalculateFlutterWave(float phase)
	{
		float windAngle = FlutterPhaseScale * phase;
		float windAngleSin, windAngleCos;
		sincos(windAngle, windAngleSin, windAngleCos);

		float windTmp3 = 0.2 * cos(Math::PI * windAngleCos);
		float windTmp1 = sin(Math::PI * windAngleSin);
		float windTmp2 = sin(Math::TAU * windAngleSin);
		return (windTmp1 + windTmp2) * 0.3 + windTmp3;
	}

	float CalculateFlutterWave(float2 instanceCoordinates, float windTimer)
	{
		return CalculateFlutterWave((instanceCoordinates.x + instanceCoordinates.y) * -0.0078125 + windTimer);
	}

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

	float3 CalculateVanillaDisplacement(
		float2 instanceCoordinates, float tipWeight, float3 windVector, float windTimer, float windIntensityScale)
	{
		float windPower = windVector.z * windIntensityScale *
		                  (CalculateFlutterWave(instanceCoordinates, windTimer) * (0.5 * (tipWeight * tipWeight)));

		return float3(windVector.xy, 0) * windPower;
	}

	float3 CalculateFlutterDisplacement(
		float flutterWeight, float3 relativePosition, float3 bendAxis, float bendAngle,
		float3 flutterAxis, float flutter,
		float maximumTiltRadians, out float flutterAngle)
	{
		flutterAngle = 0.0f;
		if (flutterWeight <= 0.0f || abs(flutter) <= EPSILON_WIND_RESPONSE ||
			dot(relativePosition, relativePosition) <= EPSILON_WIND_LENGTH_SQ)
			return 0.0f.xxx;
		float3 bentUp = RotateVector(float3(0.0f, 0.0f, 1.0f), bendAxis, bendAngle);
		float3 bentTangent = RotateVector(cross(flutterAxis, float3(0.0f, 0.0f, 1.0f)), bendAxis, bendAngle);
		float verticalRadius = max(length(float2(bentUp.z, bentTangent.z)), EPSILON_WIND_RESPONSE);
		// Symmetric headroom prevents the tilt limit from biasing flutter against the spring pose.
		float angularHeadroom = max(acos(clamp(cos(max(maximumTiltRadians, 0.0f)) / verticalRadius, -1.0f, 1.0f)) -
										atan2(abs(bentTangent.z), bentUp.z),
			0.0f);
		float maximumFlutter = min(FlutterMaximumAngleRadians, angularHeadroom);
		if (maximumFlutter <= EPSILON_WIND_RESPONSE)
			return 0.0f.xxx;
		flutterAngle = maximumFlutter * tanh(
											flutter * flutterWeight * FlutterResponseRadians / maximumFlutter);
		float3 localDisplacement = RotateVector(relativePosition, flutterAxis, flutterAngle) - relativePosition;
		return RotateVector(localDisplacement, bendAxis, bendAngle);
	}

	/** @brief Deforms a grass vertex from an already-resolved rigid bend. */
	float3 CalculateAmbientDisplacement(
		float tipWeight, float modelHeight, float instanceBaseHeight, float3 bendAxis,
		float rigidBendAngle, float rigidCompression, out float bendAngle)
	{
		float deformationWeight = saturate(tipWeight);
		bendAngle = rigidBendAngle * deformationWeight;
		float compression = saturate(rigidCompression * deformationWeight);

		float3 relativePosition = float3(0.0, 0.0, max(modelHeight - instanceBaseHeight, 0.0));
		float3 deformedPosition = RotateVector(relativePosition, bendAxis, bendAngle);
		deformedPosition.z *= 1.0 - compression;
		return deformedPosition - relativePosition;
	}
}

#endif  // __GRASS_WIND_DEPENDENCY_HLSL__
