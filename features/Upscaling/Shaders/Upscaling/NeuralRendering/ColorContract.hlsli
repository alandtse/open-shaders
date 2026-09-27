#ifndef __NR_COLOR_CONTRACT_HLSLI__
#define __NR_COLOR_CONTRACT_HLSLI__

// Composite side of the NR colour contract. Include after Common/Color.hlsli: ENABLE_LL and
// ENABLE_ACEScg read the b6 FeatureData block the caller binds.
namespace NR
{
	// Tone clamp for the composite gain, in stops; bounded so a bad model frame cannot blow out
	// the frame.
	static const float kMaxToneStops = 2.0f;

	// Bounded scalar gain for the composite: exp2(clamp(tone)) in the source's own domain, so a
	// gain of 1 is an exact passthrough and the legacy gamma path only re-encodes the gain.
	float CompositeGain(float tone, float maxToneStops)
	{
		tone = isfinite(tone) ? tone : 0.0;
		float gain = exp2(clamp(tone, -maxToneStops, maxToneStops));
		return ENABLE_LL ? gain : Color::LinearToSkyrimGamma(gain);
	}
}

#endif  // __NR_COLOR_CONTRACT_HLSLI__
