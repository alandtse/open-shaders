#ifndef TRANSIENT_WIND_TYPES_HLSLI
#define TRANSIENT_WIND_TYPES_HLSLI

namespace WindField
{
	// Keep this mirrored with WindField::kTransientImpulseCapacity; use spatial indexing or
	// a rasterized shared field if source counts make per-sample scans expensive.
	static const uint TransientImpulseCapacity = 96u;

	struct TransientWindSource
	{
		float3 origin;
		float wavefrontDistance;
		float3 direction;
		float strength;
		float maxDistance;
		float waveHalfWidth;
		float propagationSpeed;
		float coneSpreadTangent;
		float decayTime;
		float collisionRadius;
		float sideSpreadTangent;
		float sideStrength;
		uint type;
		float verticalScale;
		float falloffWidth;
		float padding;
	};
}

#endif
