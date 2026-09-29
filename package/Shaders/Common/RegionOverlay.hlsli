#ifndef REGION_OVERLAY_HLSLI
#define REGION_OVERLAY_HLSLI

namespace RegionOverlay
{
	// A region measured for one frame size can be composited into another after a render-size change,
	// which would draw it off-frame.
	uint4 ClampToFrame(uint4 rect, uint2 frame)
	{
		uint4 clamped = uint4(0, 0, 0, 0);
		if (rect.z != 0 && rect.w != 0 && frame.x != 0 && frame.y != 0 && rect.x < frame.x && rect.y < frame.y)
			clamped = uint4(rect.x, rect.y, min(rect.z, frame.x - rect.x), min(rect.w, frame.y - rect.y));
		return clamped;
	}

	float OutlineCoverage(uint2 pixel, uint4 rect, float thickness)
	{
		float coverage = 0.0;
		const bool inside = pixel.x >= rect.x && pixel.x < rect.x + rect.z &&
		                    pixel.y >= rect.y && pixel.y < rect.y + rect.w;
		if (rect.z != 0 && rect.w != 0 && thickness > 0.0 && inside) {
			const uint dx = min(pixel.x - rect.x, rect.x + rect.z - 1 - pixel.x);
			const uint dy = min(pixel.y - rect.y, rect.y + rect.w - 1 - pixel.y);
			coverage = float(min(dx, dy)) < thickness ? 1.0 : 0.0;
		}
		return coverage;
	}

	float3 OutlineOnly(float3 color, uint2 pixel, uint4 rect, float3 outlineColor, float thickness)
	{
		float3 result = color;
		if (rect.z != 0 && rect.w != 0)
			result = lerp(color, outlineColor, OutlineCoverage(pixel, rect, thickness));
		return result;
	}
}

#endif
