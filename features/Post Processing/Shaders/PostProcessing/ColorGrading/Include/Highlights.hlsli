#ifndef POST_PROCESSING_HIGHLIGHTS_HLSLI
#define POST_PROCESSING_HIGHLIGHTS_HLSLI

#include "Common/Math.hlsli"

namespace Highlights
{
	/// Applies highlight gain and offset with smooth compression when reducing brightness.
	float3 Apply(float3 color, float luma, float3 midtonesGain, float3 highlightsGain,
		float3 midtonesOffset, float3 highlightsOffset, float highlightBegin, float highlightEnd)
	{
		float begin = max(highlightBegin, 0.0f);
		float width = max(highlightEnd - begin, EPSILON_DIVISION);
		float t = saturate((luma - begin) / width);
		float weight = t * t * (3.0f - 2.0f * t);
		// Integrating the highlight mask keeps the reduced gain's brightness slope nonnegative.
		float integral = width * t * t * t * (1.0f - 0.5f * t) + max(luma - (begin + width), 0.0f);
		float integratedWeight = integral / max(luma, EPSILON_DIVISION);
		float3 gainWeight = highlightsGain < midtonesGain ? integratedWeight : weight;
		float3 gain = midtonesGain + (highlightsGain - midtonesGain) * gainWeight;
		float3 offsetDelta = highlightsOffset - midtonesOffset;
		float3 offset = midtonesOffset + max(offsetDelta, 0.0f) * weight;
		[branch] if (any(offsetDelta < 0.0f))
		{
			float transitionIntegral = width * t * t * t *
			                           (1.0f - 0.5f * t - t * t * (9.0f / 5.0f - 2.0f * t + 4.0f / 7.0f * t * t));
			// The offset's available brightness grows no faster than the gained color.
			float3 available = max(color / max(luma, EPSILON_DIVISION), 0.0f) *
			                   (min(midtonesGain, highlightsGain) * integral + max(midtonesGain - highlightsGain, 0.0f) * transitionIntegral);
			float3 reduction = max(-offsetDelta, 0.0f);
			offset -= reduction * available / max(reduction + available, EPSILON_DIVISION);
		}
		return color * gain + offset;
	}
}

#endif
