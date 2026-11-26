#ifndef NATIVE_SHADOW_SAMPLING_HLSLI
#define NATIVE_SHADOW_SAMPLING_HLSLI

#include "NativeTraceDensity.hlsli"

#ifdef __cplusplus
namespace DX12Native
{
#endif

	// Estimate emitter size independently of the light's illumination reach.
	static const float ShadowSourceRadiusFraction = 0.025f;
	static const float ShadowMinimumSourceRadius = 0.5f;
	static const float ShadowMaximumRelativeRadius = 0.5f;
	static const uint ShadowEmitterRadialSalt = 0x68bc21ebu;
	static const uint ShadowEmitterAngularSalt = 0x02e5be93u;

#ifdef __cplusplus
	inline
#endif
	float NativeShadowEmitterRadius(float lightRadius, float sourceRadius, float multiplier, float distance)
	{
		const float estimated = lightRadius * ShadowSourceRadiusFraction;
		const float source = sourceRadius > 0.0f ? sourceRadius : estimated;
		const float physicalRadius = source > ShadowMinimumSourceRadius ? source : ShadowMinimumSourceRadius;
		const float scaledRadius = physicalRadius * multiplier;
		const float maximumRadius = distance * ShadowMaximumRelativeRadius;
		return scaledRadius < maximumRadius ? scaledRadius : maximumRadius;
	}

#ifdef __cplusplus
	inline
#endif
	uint NativeShadowEmitterSeed(uint x, uint y, uint frame, uint lightID)
	{
		return NativeDensityHash(x * 0x9e3779b9u ^ y * 0x85ebca6bu ^
		                         frame * 0xc2b2ae35u ^ lightID * 0x27d4eb2fu);
	}

#ifdef __cplusplus
	inline
#endif
	float NativeShadowEmitterRandom(uint seed, uint salt)
	{
		// 23 bits plus a half step stay strictly within (0, 1) in FP32.
		return (float(NativeDensityHash(seed ^ salt) >> 9u) + 0.5f) / 8388608.0f;
	}

#ifdef __cplusplus
}
#endif
#endif
