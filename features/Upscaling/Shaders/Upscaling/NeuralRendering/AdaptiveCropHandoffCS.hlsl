// Full-display/SBS handoff for adaptive foveated crop changes.
//
// The foveated crop is a resource-boundary change, so the renderer cannot
// continuously resize the DLSS input every frame. Instead, it changes one
// settled crop tier and uses the previous completed SBS image as a short-lived
// display-space bridge. Motion and depth gates reject stale history around
// moving or disoccluded geometry; the eye-boundary clamp prevents left/right
// reprojection from crossing the stereo split.

cbuffer AdaptiveCropHandoffParams : register(b0)
{
	uint gColorWidth;
	uint gColorHeight;
	uint gGuideWidth;
	uint gGuideHeight;
	float gMotionScaleX;
	float gMotionScaleY;
	float gBlendAlpha;
	float gDepthThreshold;
	uint gHistoryValid;
	uint gUseDepth;
	uint gUseMotion;
	uint gPadding0;
};

Texture2D<float4> gCurrent : register(t0);
Texture2D<float4> gPrevious : register(t1);
Texture2D<float> gCurrentDepth : register(t2);
Texture2D<float> gPreviousDepth : register(t3);
Texture2D<float2> gMotion : register(t4);
RWTexture2D<float4> gTarget : register(u0);
SamplerState gLinear : register(s0);

uint2 GuidePixel(float2 colorPixel)
{
	const float2 colorSize = float2(max(gColorWidth, 1u), max(gColorHeight, 1u));
	const float2 guideSize = float2(max(gGuideWidth, 1u), max(gGuideHeight, 1u));
	const float2 uv = (colorPixel + 0.5) / colorSize;
	return min(uint2(max(uv * guideSize, 0.0)),
		uint2(max(gGuideWidth, 1u) - 1u, max(gGuideHeight, 1u) - 1u));
}

float2 MotionPixels(uint2 colorPixel)
{
	if (gUseMotion == 0)
		return 0.0;
	const uint2 guidePixel = GuidePixel(float2(colorPixel));
	const float2 motion = gMotion.Load(int3(guidePixel, 0));
	const float2 guideToColor = float2(
		(float)gColorWidth / max((float)gGuideWidth, 1.0),
		(float)gColorHeight / max((float)gGuideHeight, 1.0));
	return motion * float2(gMotionScaleX, gMotionScaleY) * guideToColor;
}

bool InBounds(float2 pixel)
{
	return all(pixel >= 0.5) && pixel.x < (float)gColorWidth - 0.5 &&
	       pixel.y < (float)gColorHeight - 0.5;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= gColorWidth || id.y >= gColorHeight)
		return;

	const uint2 pixel = id.xy;
	const float4 current = gCurrent.Load(int3(pixel, 0));
	const float2 motionPixels = MotionPixels(pixel);
	const float2 outputSize = float2(max(gColorWidth, 1u), max(gColorHeight, 1u));
	float2 previousPosition = float2(pixel) + 0.5 + motionPixels;
	bool historyAccepted = gHistoryValid != 0 && InBounds(previousPosition);

	// Do not let a per-eye motion vector sample across the SBS seam.
	const float halfWidth = 0.5 * (float)gColorWidth;
	const float eyeMinX = (float)pixel.x < halfWidth ? 0.0 : halfWidth;
	const float eyeMaxX = eyeMinX + halfWidth;
	previousPosition.x = clamp(previousPosition.x, eyeMinX + 0.5, eyeMaxX - 0.5);
	previousPosition.y = clamp(previousPosition.y, 0.5, (float)gColorHeight - 0.5);

	if (historyAccepted && gUseDepth != 0) {
		const uint2 currentGuidePixel = GuidePixel(float2(pixel));
		const uint2 previousGuidePixel = GuidePixel(previousPosition);
		const float currentDepth = gCurrentDepth.Load(int3(currentGuidePixel, 0));
		const float previousDepth = gPreviousDepth.Load(int3(previousGuidePixel, 0));
		const bool currentDepthValid = currentDepth > 0.00001;
		const bool previousDepthValid = previousDepth > 0.00001;
		if (currentDepthValid && previousDepthValid) {
			const float depthScale = max(max(abs(currentDepth), abs(previousDepth)), 1.0);
			historyAccepted = abs(currentDepth - previousDepth) <= gDepthThreshold * depthScale;
		}
	}

	const float4 previous = gPrevious.SampleLevel(gLinear, previousPosition / outputSize, 0);
	const float normalizedMotion = length(motionPixels / max(outputSize, 1.0));
	const float motionAlpha = saturate(normalizedMotion * 10.0);
	const float alpha = historyAccepted ? max(saturate(gBlendAlpha), motionAlpha * motionAlpha) : 1.0;
	gTarget[pixel] = lerp(previous, current, alpha);
}
