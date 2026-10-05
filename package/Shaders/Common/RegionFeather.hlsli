#ifndef REGION_FEATHER_HLSLI
#define REGION_FEATHER_HLSLI

namespace RegionFeather
{
	// The band is the free margin between the subject and the crop edge, so the weight is 1 by the subject's box.
	float SideWidth(float margin, float minWidth, float maxWidth)
	{
		return clamp(margin, minWidth, maxWidth);
	}

	// Rectangles are (left, top, right, bottom) in eye pixels; a subject with no extent falls back to defaultWidth.
	// An edge on the frame edge has no band: there is no un-enhanced neighbour to blend with.
	float Weight(float2 position, float4 crop, float4 subject, float2 frame, float defaultWidth, float minWidth, float maxWidth)
	{
		const bool hasSubject = subject.z > subject.x && subject.w > subject.y;
		const float4 margins = float4(subject.x - crop.x, subject.y - crop.y, crop.z - subject.z, crop.w - subject.w);
		const float4 fallback = float4(defaultWidth, defaultWidth, defaultWidth, defaultWidth);
		const float4 widths = hasSubject ?
		                          float4(SideWidth(margins.x, minWidth, maxWidth), SideWidth(margins.y, minWidth, maxWidth),
									  SideWidth(margins.z, minWidth, maxWidth), SideWidth(margins.w, minWidth, maxWidth)) :
		                          fallback;
		const float4 distances = float4(position.x - crop.x, position.y - crop.y, crop.z - position.x, crop.w - position.y);
		const float4 onFrameEdge = float4(crop.x <= 0.0, crop.y <= 0.0, crop.z >= frame.x, crop.w >= frame.y);
		const float4 progress = lerp(saturate(distances / widths), float4(1.0, 1.0, 1.0, 1.0), onFrameEdge);
		return smoothstep(0.0, 1.0, min(min(progress.x, progress.y), min(progress.z, progress.w)));
	}
}

#endif
