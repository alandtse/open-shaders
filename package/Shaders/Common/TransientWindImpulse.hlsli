#ifndef __TRANSIENT_WIND_IMPULSE_DEPENDENCY_HLSL__
#define __TRANSIENT_WIND_IMPULSE_DEPENDENCY_HLSL__

namespace WindField
{
	// Keep this mirrored with WindField::kTransientImpulseCapacity; use spatial indexing or
	// a rasterized shared field if source counts make per-sample scans expensive.
	static const uint TransientImpulseCapacity = 24u;

	struct TransientImpulse
	{
		float3 origin;
		float wavefrontDistance;
		float3 direction;
		float strength;
		float maxDistance;
		float waveHalfWidth;
		float propagationSpeed;
		float coneCosine;
		float decayTime;
		float3 padding;
	};

	struct TransientImpulseSample
	{
		float3 velocity;
		float intensity;
	};

	/** @brief Samples one moving directional pressure wave and its weakening trailing wake. */
	TransientImpulseSample SampleTransientImpulse(float3 worldPosition, TransientImpulse impulse)
	{
		TransientImpulseSample sample;
		sample.velocity = 0.0f;
		sample.intensity = 0.0f;
		if (impulse.strength <= 0.0f || impulse.maxDistance <= 0.0f)
			return sample;

		float directionLengthSquared = dot(impulse.direction, impulse.direction);
		float3 direction = directionLengthSquared > 1e-6f ?
		                       impulse.direction / sqrt(directionLengthSquared) :
		                       0.0f;
		float3 offset = worldPosition - impulse.origin;
		float distanceSquared = dot(offset, offset);
		float distance = sqrt(max(distanceSquared, 0.0f));
		if (distance > impulse.maxDistance)
			return sample;
		float alignment = distanceSquared > 1e-6f ? dot(offset, direction) / distance : 1.0f;
		float angularFalloff = smoothstep(clamp(impulse.coneCosine, -1.0f, 1.0f), 1.0f, alignment);
		float waveHalfWidth = max(abs(impulse.waveHalfWidth), 1e-4f);
		float waveDistance = max(impulse.wavefrontDistance, 0.0f);
		float distanceFromWavefront = distance - waveDistance;
		float trailingWidth = waveHalfWidth +
		                      max(impulse.propagationSpeed, 0.0f) * clamp(impulse.decayTime, 0.0f, 5.0f);
		float localWaveWidth = distanceFromWavefront < 0.0f ? trailingWidth : waveHalfWidth;
		float waveFalloff = 1.0f - smoothstep(0.0f, localWaveWidth, abs(distanceFromWavefront));
		float distanceFalloff = 1.0f - smoothstep(0.0f, impulse.maxDistance, distance);
		sample.intensity = saturate(angularFalloff * waveFalloff * distanceFalloff);
		sample.velocity = direction * (impulse.strength * sample.intensity);
		return sample;
	}
}

#endif  // __TRANSIENT_WIND_IMPULSE_DEPENDENCY_HLSL__
