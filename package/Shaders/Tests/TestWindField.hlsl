#include "/Shaders/Common/WindField.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

WindField::WindTuning GetDefaultWindTuning()
{
	WindField::WindTuning tuning;
	tuning.gustScale = 2048.0f;
	tuning.frontAspectRatio = 4.0f;
	tuning.gustAdvectionBaseSpeed = 384.0f;
	tuning.gustAdvectionMultiplier = 1.0f;
	tuning.detailScaleRatio = 0.38f;
	tuning.detailCrosswindScaleRatio = 0.55f;
	tuning.turbulenceStrength = 0.24f;
	tuning.turbulenceSkew = 0.35f;
	tuning.contrastLow = 0.30f;
	tuning.contrastHigh = 0.70f;
	tuning.gustAmplitude = 0.35f;
	tuning.broadGustSeed = 0x2341316Cu;
	tuning.turbulentGustSeed = 0x48013EA4u;
	tuning.gradientSeedMix = 0x1E3779B9u;
	tuning.pcgMultiplier = 1664525u;
	tuning.pcgIncrement = 1013904223u;
	return tuning;
}

/// @tags wind-field, cpu-gpu-parity
[numthreads(1, 1, 1)] void TestWindFieldCpuGpuParitySamples() {
	static const float parityTolerance = 5e-4f;
	WindField::WindTuning tuning = GetDefaultWindTuning();

#define WIND_FIELD_PARITY_SAMPLE(                                                                                                \
	positionX, positionY, positionZ, travelDistance, directionX, directionY, directionZ, speed, expectedGust, expectedVelocityX, \
	expectedVelocityY, expectedVelocityZ)                                                                                        \
	{                                                                                                                            \
		float3 position = float3(positionX, positionY, positionZ);                                                               \
		float3 direction = float3(directionX, directionY, directionZ);                                                           \
		WindField::WindSample sample = WindField::SampleWind(position, travelDistance, direction, speed, tuning);                \
		float3 expectedVelocity = float3(expectedVelocityX, expectedVelocityY, expectedVelocityZ);                               \
		ASSERT(IsTrue, abs(sample.ambientGust - expectedGust) < parityTolerance);                                                \
		ASSERT(IsTrue, all(abs(sample.velocity - expectedVelocity) < parityTolerance));                                          \
	}
#include "/Shaders/Tests/WindFieldParitySamples.hlsli"
#undef WIND_FIELD_PARITY_SAMPLE
}
