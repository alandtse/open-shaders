#ifndef __NR_COLOR_CONTRACT_HLSLI__
#define __NR_COLOR_CONTRACT_HLSLI__

#include "Common/Color.hlsli"

// Colour contract of the neural-rendering proxy: how kMAIN is linearized for the model and the
// exposure estimate, how the model's bounded input proxy is encoded, and the scalar gain the
// composite applies in the source's own domain. The CPU twin of these constants and functions is
// NeuralRendering/ColorContract.h; a change to either side must change both.
namespace NR
{
	// Rec.709 luma weights, applied to sRGB-decoded proxy values even under ACEScg: the proxy is
	// sRGB-primaries after AP1TosRGB, so it is not a working-space luminance.
	static const float3 Luma = float3(0.2126, 0.7152, 0.0722);

	static const float kProxyEpsilon = 1e-8;
	static const float kProxyPeakEpsilon = 1e-6;
	static const float kLumaFloor = 1e-5;
	static const float kDepthEpsilon = 1e-4;
	static const float kLegacySceneGamma = 1.6;
	static const float kExposureLumaFloor = 1e-4;

	float3 ProxyLinearToSrgb(float3 value)
	{
		value = saturate(value);
		return lerp(value * 12.92, 1.055 * pow(max(value, kProxyEpsilon), 1.0 / 2.4) - 0.055, step(0.0031308, value));
	}

	float3 ProxySrgbToLinear(float3 value)
	{
		value = saturate(value);
		return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4), step(0.04045, value));
	}

	// Max-channel compression from zero; bounded to [0,1) but not luminance-preserving.
	float3 NeutwoEncode(float3 value)
	{
		value = max(value, 0.0);
		float peak = max(value.r, max(value.g, value.b));
		if (peak <= kProxyPeakEpsilon)
			return value;
		return value * ((peak * rsqrt(peak * peak + 1.0)) / peak);
	}

	// Linearization for the proxy and the exposure estimate only; the composite never uses it.
	float3 ToLinearNR(float3 value)
	{
		return ENABLE_LL ? (ENABLE_ACEScg ? AP1TosRGB(value) : value) : Color::SkyrimGammaToLinear(max(value, 0.0));
	}

	float3 MakeDisplayProxy(float3 linearColor)
	{
		return ProxyLinearToSrgb(NeutwoEncode(linearColor));
	}

	// Bounded scalar gain for the composite: exp2(clamp(tone)) in the source's own domain, so a
	// gain of 1 is an exact passthrough and the legacy gamma path only re-encodes the gain.
	float CompositeGain(float tone, float maxToneStops)
	{
		float gain = exp2(clamp(tone, -maxToneStops, maxToneStops));
		// 1/1.6 is exactly representable, so this cannot perturb the gain = 1 passthrough.
		return ENABLE_LL ? gain : pow(abs(gain), 1.0 / kLegacySceneGamma);
	}

	// Geometric-mean luminance of one linearized sample, floored so a black pixel cannot drive
	// the mean to -inf and leave the following frame with a zero exposure. The linearization leaves
	// the value in sRGB primaries even under ACEScg, so it takes NR::Luma, not the AP1 weights.
	float ExposureLogLuminance(float3 source)
	{
		return log2(max(dot(ToLinearNR(source), Luma), kExposureLumaFloor));
	}
}

#endif  // __NR_COLOR_CONTRACT_HLSLI__
