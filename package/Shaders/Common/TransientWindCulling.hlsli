#ifndef TRANSIENT_WIND_CULLING_HLSLI
#define TRANSIENT_WIND_CULLING_HLSLI

#include "Common/TransientWindImpulse.hlsli"

namespace TransientWindCulling
{
	// 3.5*sqrt(2): tileRadius factor for the 8x8-cell tiles both GrassWindSpringCS.hlsl and
	// TreeWindSpringCS.hlsl dispatch in (their [numthreads(8,8,1)] groups own one tile each).
	static const float kEightCellTileRadiusFactor = 4.94974747f;

	bool SourceMayAffectTile(
		WindField::TransientWindSource source, float2 tileCenter, float tileRadius)
	{
		if (source.strength <= 0.0f || source.maxDistance <= 0.0f)
			return false;

		// This bound covers every analytic source shape without rejecting valid cells.
		float lateralSpread = max(source.coneSpreadTangent, max(source.sideSpreadTangent, 0.0f));
		float influenceRadius = source.maxDistance + max(abs(source.collisionRadius), 0.0f) +
		                        source.maxDistance * lateralSpread +
		                        max(abs(source.propagationSpeed), abs(source.waveHalfWidth)) + tileRadius;
		float2 offset = tileCenter - source.origin.xy;
		return dot(offset, offset) <= influenceRadius * influenceRadius;
	}
}

#endif
