#ifndef __WIND_FIELD_TYPES_DEPENDENCY_HLSL__
#define __WIND_FIELD_TYPES_DEPENDENCY_HLSL__

namespace WindField
{
	struct WindTuning
	{
		// Supplied by the canonical C++ WindTuning; the GPU implementation defines no defaults.
		float gustScale;
		float frontAspectRatio;
		float gustAdvectionBaseSpeed;
		float gustAdvectionMultiplier;
		float detailScaleRatio;
		float detailCrosswindScaleRatio;
		float turbulenceStrength;
		float turbulenceSkew;
		float contrastLow;
		float contrastHigh;
		float gustAmplitude;
		uint broadGustSeed;
		uint turbulentGustSeed;
		uint gradientSeedMix;
		uint pcgMultiplier;
		uint pcgIncrement;
		float distortionStrength;
		float distortionScale;
		float distortionSpeed;
		float microTurbulenceStrength;
		float fineBaselineAmplitude;
		float fineWindCoupling;
		float fineGustCoupling;
		float fineFrequency;
	};

	struct FineState
	{
		float4 travel;
	};

	struct FineSample
	{
		float3 slowVelocity;
		float3 detailVelocity;
	};

	struct WindSample
	{
		float3 velocity;
		float ambientGust;
		float ambientTurbulence;
		float transientImpulse;
	};

	struct Components
	{
		float3 baseAmbientVelocity;
		float3 gustVelocity;
		float3 microVelocity;
		float3 slowMicroVelocity;
		float3 detailMicroVelocity;
		float3 transientVelocity;
		float ambientGust;
		float ambientTurbulence;
		float transientImpulse;
	};

	struct Field
	{
		float3 direction;
		float travelDistance;
		float3 crosswind;
		float speed;
	};
}

#endif  // __WIND_FIELD_TYPES_DEPENDENCY_HLSL__
