#ifndef __GRASS_WIND_RESPONSE_HLSLI__
#define __GRASS_WIND_RESPONSE_HLSLI__

#include "Common/GrassWind.hlsli"
#include "Common/GrassWindSpring.hlsli"
#include "Common/Random.hlsli"

namespace GrassWindResponse
{
	void Sample(float2 instanceCoordinates, float2 rootWorldPosition, float2 previousRootWorldPosition,
		float4x4 worldMatrix, float4x4 previousWorldMatrix, float windTimer, float previousWindTimer,
		out float4 currentResponse, out float4 previousResponse, out float2 flutter)
	{
		currentResponse = previousResponse = 0.0f.xxxx;
		float intensityScale = GrassWind::GetWindIntensityOverrideScale();
		flutter = float2(
					  GrassWind::CalculateFlutterWave(instanceCoordinates, windTimer),
					  GrassWind::CalculateFlutterWave(instanceCoordinates, previousWindTimer)) *
		          intensityScale;
		if (Permutation::EnableAmbientGrassWind != 0) {
			uint currentField = GrassWindSpring::SelectField(rootWorldPosition);
			uint previousField = GrassWindSpring::SelectField(previousRootWorldPosition);
			if (currentField == previousField && GrassWindSpring::HasTemporalCoverage(
													 currentField, previousField, rootWorldPosition, previousRootWorldPosition)) {
				float4 currentSample = GrassWindSpring::SampleCurrent(currentField, rootWorldPosition);
				float4 previousSample = GrassWindSpring::SamplePrevious(previousField, previousRootWorldPosition);
				float responseScale = lerp(0.9, 1.1, Random::InterleavedGradientNoise(instanceCoordinates));
				float3 currentAxis, previousAxis;
				GrassWindSpring::ResolveModelBend(currentSample, responseScale, worldMatrix,
					GrassWindSpring::Fields[currentField].MaximumTiltRadians,
					Permutation::GrassWindCompressionToBend, currentAxis, currentResponse.z, currentResponse.w);
				GrassWindSpring::ResolveModelBend(previousSample, responseScale, previousWorldMatrix,
					GrassWindSpring::Fields[previousField].MaximumTiltRadians,
					Permutation::GrassWindCompressionToBend, previousAxis, previousResponse.z, previousResponse.w);
				currentResponse.xy = currentAxis.xy;
				previousResponse.xy = previousAxis.xy;
				float fieldStrength = max(Permutation::GrassWindFlutterStrength, 0.0f) * intensityScale;
				flutter = float2(currentSample.w, previousSample.w) * fieldStrength;
			}
		}
	}
}

#endif
