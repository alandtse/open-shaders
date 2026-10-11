#ifndef __NR_COLOR_CONTRACT_HLSLI__
#define __NR_COLOR_CONTRACT_HLSLI__

// Include after Common/Color.hlsli: ENABLE_LL selects the gain's output domain.
namespace NR
{
	// Tone clamp in stops: bounded so a bad model frame cannot blow out the frame.
	static const float kMaxToneStops = 2.0f;

	// exp2 of the tone in the source's own domain: tone 0 stays an exact gain-1 passthrough.
	float CompositeGain(float tone, float brighteningWeight, float darkeningWeight,
		float maxBrighteningStops, float maxDarkeningStops)
	{
		tone = isfinite(tone) ? tone : 0.0;
		brighteningWeight = isfinite(brighteningWeight) ? clamp(brighteningWeight, 0.0, 2.0) : 1.0;
		darkeningWeight = isfinite(darkeningWeight) ? clamp(darkeningWeight, 0.0, 2.0) : 1.0;
		maxBrighteningStops = isfinite(maxBrighteningStops) ? clamp(maxBrighteningStops, 0.0, kMaxToneStops) : kMaxToneStops;
		maxDarkeningStops = isfinite(maxDarkeningStops) ? clamp(maxDarkeningStops, 0.0, kMaxToneStops) : kMaxToneStops;
		tone *= tone >= 0.0 ? brighteningWeight : darkeningWeight;
		float gain = exp2(clamp(tone, -maxDarkeningStops, maxBrighteningStops));
		return ENABLE_LL ? gain : Color::LinearToSkyrimGamma(gain);
	}
}

#endif  // __NR_COLOR_CONTRACT_HLSLI__
