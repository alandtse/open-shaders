#ifndef __NR_COLOR_CONTRACT_HLSLI__
#define __NR_COLOR_CONTRACT_HLSLI__

#include "Common/Color.hlsli"

// ENABLE_LL (from Common/Color.hlsli) selects the gain's output domain.
namespace NR
{
	// Tone clamp in stops: bounded so a bad model frame cannot blow out the frame.
	static const float kMaxToneStops = 2.0f;

	// exp2 of the tone in the source's own domain: tone 0 stays an exact gain-1 passthrough.
	float CompositeGain(float tone, float maxToneStops)
	{
		tone = isfinite(tone) ? tone : 0.0;
		float gain = exp2(clamp(tone, -maxToneStops, maxToneStops));
		return ENABLE_LL ? gain : Color::LinearToSkyrimGamma(gain);
	}

	static const float kMaxChromaStops = 1.0f;
	static const float kChromaFloor = 1.0f / 512.0f;
	static const float kChromaTaperStart = 0.85f;
	static const float kChromaTaperEnd = 0.98f;

	// The proxy clips at 1, so a channel near it carries no usable colour offset; fade the chroma out before it does.
	float ChromaHeadroom(float3 proxy)
	{
		return 1.0 - smoothstep(kChromaTaperStart, kChromaTaperEnd, max(proxy.r, max(proxy.g, proxy.b)));
	}

	// Luminance-neutral, so CompositeGain stays the only brightness edit.
	float3 ChromaOffset(float3 inputLinear, float3 outputLinear, float3 lumaWeights, float floorValue)
	{
		float3 offset = log2((max(outputLinear, 0.0) + floorValue) / (max(inputLinear, 0.0) + floorValue));
		return offset - dot(lumaWeights, offset);
	}

	float3 ChromaFromRedBlue(float2 redBlue, float3 lumaWeights)
	{
		return float3(redBlue.x, -(lumaWeights.x * redBlue.x + lumaWeights.z * redBlue.y) / lumaWeights.y, redBlue.y);
	}

	// Linear-light gain, bounded by the stop limit.
	float3 LinearChromaGain(float3 offset, float strength, float maxChromaStops)
	{
		float3 scaled = all(isfinite(offset)) ? offset * strength : float3(0.0, 0.0, 0.0);
		return exp2(clamp(scaled, -maxChromaStops, maxChromaStops));
	}

	// A zero-mean log offset still shifts luminance, so rescale it and then shrink it toward 1 to hold the stop
	// bound; the last pass shrinks, so the bound is exact and luminance is within about 1e-4.
	float3 LuminancePreservingGain(float3 baseLinear, float3 gain, float3 lumaWeights, float maxChromaStops)
	{
		float before = dot(lumaWeights, baseLinear);
		float3 kept = gain;
		[unroll] for (uint i = 0; i < 12; ++i)
		{
			float after = dot(lumaWeights, baseLinear * kept);
			if (before > 1e-6 && after > 1e-6)
				kept *= before / after;
			float3 stops = log2(kept);
			float excess = max(max(abs(stops.x), abs(stops.y)), abs(stops.z));
			if (excess > maxChromaStops)
				kept = exp2(stops * (maxChromaStops / excess));
		}
		return kept;
	}

	float3 ChromaGain(float3 offset, float strength, float maxChromaStops)
	{
		float3 gain = LinearChromaGain(offset, strength, maxChromaStops);
		return ENABLE_LL ? gain : Color::LinearToSkyrimGamma(gain);
	}

	// Game-domain gain for a game-domain colour: the rescale happens in linear light in either domain.
	float3 CompositeChromaGain(float3 gameColor, float3 offset, float strength, float3 lumaWeights, float maxChromaStops)
	{
		float3 baseLinear = ENABLE_LL ? gameColor : Color::SkyrimGammaToLinear(gameColor);
		float3 gain = LuminancePreservingGain(baseLinear, LinearChromaGain(offset, strength, maxChromaStops), lumaWeights, maxChromaStops);
		return ENABLE_LL ? gain : Color::LinearToSkyrimGamma(gain);
	}
}

#endif  // __NR_COLOR_CONTRACT_HLSLI__
