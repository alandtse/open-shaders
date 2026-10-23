#ifndef __GRASS_WIND_RESPONSE_HLSLI__
#define __GRASS_WIND_RESPONSE_HLSLI__

#include "Common/GrassWind.hlsli"
#include "Common/GrassWindSpring.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

namespace GrassWindResponse
{
	static const float FieldFadeStart = 0.85f;

	void Sample(float2 instanceCoordinates, float2 rootWorldPosition, float2 previousRootWorldPosition,
		float4x4 worldMatrix, float4x4 previousWorldMatrix, float windTimer, float previousWindTimer,
		out float4 currentResponse, out float4 previousResponse, out float2 flutter)
	{
		currentResponse = previousResponse = float4(0.0f, 0.0f, 0.0f, -1.0f);
		float intensityScale = GrassWind::GetWindIntensityOverrideScale();
		flutter = 0.0f.xx;
		if (Permutation::EnableAmbientGrassWind == 0)
			flutter = float2(
						  GrassWind::CalculateFlutterWave(instanceCoordinates, windTimer),
						  GrassWind::CalculateFlutterWave(instanceCoordinates, previousWindTimer)) *
			          intensityScale;
		if (Permutation::EnableAmbientGrassWind != 0) {
			uint currentField = GrassWindSpring::SelectField(rootWorldPosition);
			uint previousField = GrassWindSpring::SelectField(previousRootWorldPosition, true);
			float4 currentSample = 0.0f.xxxx;
			float4 previousSample = 0.0f.xxxx;
			float currentFieldWeight = 0.0f;
			float previousFieldWeight = 0.0f;
			float farDistance = GrassWindSpring::Fields[GrassWindSpring::QualityRangeCount - 1u].MaxDistance;
			if (GrassWindSpring::Fields[currentField].FieldAvailable != 0u &&
				GrassWindSpring::IsInQualityRange(currentField, rootWorldPosition) &&
				GrassWindSpring::Contains(rootWorldPosition, GrassWindSpring::Fields[currentField])) {
				float2 currentCenter = GrassWindSpring::Fields[0].FieldMinimum + GrassWindSpring::Fields[0].FieldSize * 0.5f;
				currentFieldWeight = 1.0f - smoothstep(farDistance * FieldFadeStart, farDistance,
												length(rootWorldPosition - currentCenter));
				currentSample = GrassWindSpring::SampleCurrent(currentField, rootWorldPosition);
			}
			if (GrassWindSpring::Fields[previousField].FieldAvailable != 0u &&
				GrassWindSpring::IsInQualityRange(previousField, previousRootWorldPosition, true) &&
				GrassWindSpring::Contains(previousRootWorldPosition,
					GrassWindSpring::Fields[previousField].PreviousFieldMinimum, GrassWindSpring::Fields[previousField].FieldSize)) {
				float2 previousCenter = GrassWindSpring::Fields[0].PreviousFieldMinimum + GrassWindSpring::Fields[0].FieldSize * 0.5f;
				previousFieldWeight = 1.0f - smoothstep(farDistance * FieldFadeStart, farDistance,
												 length(previousRootWorldPosition - previousCenter));
				previousSample = GrassWindSpring::SamplePrevious(previousField, previousRootWorldPosition);
			}
			[branch] if (currentFieldWeight < 1.0f)
			{
				float4 fallback = float4(GrassWindSpring::CalculateTarget(
											 SharedData::WindFieldAmbient.xyz, 0.0f.xxx, GrassWindSpring::Fields[currentField]),
					0.0f);
				currentSample = lerp(fallback, currentSample, currentFieldWeight);
			}
			[branch] if (previousFieldWeight < 1.0f)
			{
				float4 fallback = float4(GrassWindSpring::CalculateTarget(
											 SharedData::WindFieldPreviousAmbient.xyz, 0.0f.xxx, GrassWindSpring::Fields[previousField]),
					0.0f);
				previousSample = lerp(fallback, previousSample, previousFieldWeight);
			}
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

#endif
