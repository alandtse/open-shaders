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
}

#endif  // __NR_COLOR_CONTRACT_HLSLI__
