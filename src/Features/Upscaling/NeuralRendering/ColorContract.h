#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NR::Color
{
	/**
	 * @brief Constants and helpers of the neural-rendering colour contract: the CPU twin of
	 *        NeuralRendering/ColorContract.hlsli, which holds the functions the GPU runs, so a
	 *        change to one side must change the other. ToLinearNR's Linear Lighting + ACEScg gamut
	 *        conversion is not mirrored here: it is ColorSpaces' AP1-to-sRGB matrices on the GPU.
	 */

	/** @brief Rec.709 luma weights, applied to sRGB-decoded proxy values even under ACEScg. */
	inline const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

	inline constexpr float kProxyEpsilon = 1e-8f;
	inline constexpr float kProxyPeakEpsilon = 1e-6f;
	inline constexpr float kLumaFloor = 1e-5f;
	inline constexpr float kDepthEpsilon = 1e-4f;
	/** @brief Scene gamma of the legacy (non-Linear-Lighting) kMAIN encoding. */
	inline constexpr float kLegacySceneGamma = 1.6f;
	/** @brief Largest tone stop the composite may apply, bounding its gain to [0.25, 4]. */
	inline constexpr float kMaxToneStops = 2.0f;
	inline constexpr float kExposureLumaFloor = 1e-4f;
	inline constexpr float kMiddleGrey = 0.18f;
	/** @brief Adapted-luminance clamp of the local exposure estimate; exp2(-6) and exp2(2). */
	inline constexpr float kExposureMinLum = 0.015625f;
	inline constexpr float kExposureMaxLum = 4.0f;
	/** @brief Time constant of the local estimate's adaptation, in seconds. */
	inline constexpr float kExposureTau = 0.5f;

	/** @brief Temporal blend of the residual stabilizer, per frame. */
	inline constexpr float kTemporalAlpha = 0.12f;
	/** @brief Sigma multiplier of the stabilizer's neighbourhood clip radius. */
	inline constexpr float kTemporalSigmaClip = 2.0f;
	/** @brief Floor of that clip radius, so a flat neighbourhood still clips. */
	inline constexpr float kTemporalClamp = 0.05f;
	/** @brief Relative screen-depth change above which reprojected history is rejected. */
	inline constexpr float kDepthRejectThreshold = 0.02f;

	/** @brief Luminance of a linear colour, in the proxy's sRGB primaries. */
	[[nodiscard]] inline float Luminance(const float3& a_color)
	{
		return a_color.x * kLuma.x + a_color.y * kLuma.y + a_color.z * kLuma.z;
	}

	/** @brief Max-channel compression from zero; bounded to [0,1) but not luminance-preserving. */
	[[nodiscard]] inline float3 NeutwoEncode(const float3& a_value)
	{
		const float3 value = { std::max(a_value.x, 0.0f), std::max(a_value.y, 0.0f), std::max(a_value.z, 0.0f) };
		const float peak = std::max(value.x, std::max(value.y, value.z));
		if (peak <= kProxyPeakEpsilon)
			return value;
		return value / std::sqrt(peak * peak + 1.0f);
	}

	/** @brief Piecewise sRGB OETF, local to the proxy. */
	[[nodiscard]] inline float3 ProxyLinearToSrgb(const float3& a_value)
	{
		const auto encode = [](float a_channel) {
			const float value = std::clamp(a_channel, 0.0f, 1.0f);
			if (value < 0.0031308f)
				return value * 12.92f;
			return 1.055f * std::pow(std::max(value, kProxyEpsilon), 1.0f / 2.4f) - 0.055f;
		};
		return { encode(a_value.x), encode(a_value.y), encode(a_value.z) };
	}

	/** @brief Inverse of ProxyLinearToSrgb on [0,1]. */
	[[nodiscard]] inline float3 ProxySrgbToLinear(const float3& a_value)
	{
		const auto decode = [](float a_channel) {
			const float value = std::clamp(a_channel, 0.0f, 1.0f);
			if (value < 0.04045f)
				return value / 12.92f;
			return std::pow((value + 0.055f) / 1.055f, 2.4f);
		};
		return { decode(a_value.x), decode(a_value.y), decode(a_value.z) };
	}

	/**
	 * @brief Linearization for the proxy and the exposure estimate only; the composite never uses it.
	 * @param a_linearLighting ENABLE_LL from the frame's SharedData binding. With Linear Lighting on
	 *        the value is already scene-linear, so only the legacy encoding is decoded here.
	 */
	[[nodiscard]] inline float3 ToLinearNR(const float3& a_value, bool a_linearLighting)
	{
		if (a_linearLighting)
			return a_value;
		return { std::pow(std::max(a_value.x, 0.0f), kLegacySceneGamma),
			std::pow(std::max(a_value.y, 0.0f), kLegacySceneGamma),
			std::pow(std::max(a_value.z, 0.0f), kLegacySceneGamma) };
	}

	/** @brief The bounded display proxy the model consumes. */
	[[nodiscard]] inline float3 MakeDisplayProxy(const float3& a_linearColor)
	{
		return ProxyLinearToSrgb(NeutwoEncode(a_linearColor));
	}

	/** @brief The tone residual the composite multiplies, clamped to the bound it applies. */
	[[nodiscard]] inline float ClampTone(float a_tone, float a_maxToneStops)
	{
		return std::clamp(a_tone, -a_maxToneStops, a_maxToneStops);
	}

	/**
	 * @brief Scalar gain the composite multiplies kMAIN by, in kMAIN's own domain.
	 *        Exactly 1 at a tone of 0 (1/1.6 is exactly representable and the legacy path only
	 *        re-encodes the gain), so a zero residual is a passthrough in both domains.
	 */
	[[nodiscard]] inline float CompositeGain(float a_tone, float a_maxToneStops, bool a_linearLighting)
	{
		const float gain = std::exp2(ClampTone(a_tone, a_maxToneStops));
		return a_linearLighting ? gain : std::pow(std::abs(gain), 1.0f / kLegacySceneGamma);
	}

	/** @brief log2 luminance of one linearized sample, floored so a black pixel cannot reach -inf. */
	[[nodiscard]] inline float ExposureLogLuminance(const float3& a_source, bool a_linearLighting)
	{
		return std::log2(std::max(Luminance(ToLinearNR(a_source, a_linearLighting)), kExposureLumaFloor));
	}

	/** @brief Exposure from an adapted luminance; matches SceneExposure::Evaluate. */
	[[nodiscard]] inline float ExposureFromAdapted(float a_adapted, float a_minLuminance, float a_maxLuminance)
	{
		return kMiddleGrey / std::clamp(a_adapted, a_minLuminance, a_maxLuminance);
	}

	/**
	 * @brief Temporal adaptation of the local estimate's luminance.
	 * @param a_resetAdaptation Snap to the target instead of blending, for a history reset.
	 * @return The luminance to adapt from next frame.
	 */
	[[nodiscard]] inline float AdaptExposure(float a_previousAdapted, float a_target, float a_deltaTime, bool a_resetAdaptation, float a_tau)
	{
		if (a_resetAdaptation || !std::isfinite(a_previousAdapted))
			return a_target;
		const float blend = 1.0f - std::exp(-a_deltaTime / std::max(a_tau, 1e-4f));
		return a_previousAdapted + (a_target - a_previousAdapted) * blend;
	}
}
