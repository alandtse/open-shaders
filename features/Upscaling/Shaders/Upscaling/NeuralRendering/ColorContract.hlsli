#ifndef __NR_COLOR_CONTRACT_HLSLI__
#define __NR_COLOR_CONTRACT_HLSLI__

// Include after Common/Color.hlsli: ENABLE_LL selects the gain's output domain.
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

	// A zero-mean offset still moves the luminance of a coloured input, so the gain is rescaled to leave it alone.
	float3 LuminancePreservingGain(float3 baseColor, float3 gain, float3 lumaWeights)
	{
		float before = dot(lumaWeights, baseColor);
		float after = dot(lumaWeights, baseColor * gain);
		return (before > 1e-6 && after > 1e-6) ? gain * (before / after) : gain;
	}

	float3 ChromaGain(float3 offset, float strength, float maxChromaStops)
	{
		float3 scaled = all(isfinite(offset)) ? offset * strength : float3(0.0, 0.0, 0.0);
		float3 gain = exp2(clamp(scaled, -maxChromaStops, maxChromaStops));
		return ENABLE_LL ? gain : Color::LinearToSkyrimGamma(gain);
	}
}

#endif  // __NR_COLOR_CONTRACT_HLSLI__
