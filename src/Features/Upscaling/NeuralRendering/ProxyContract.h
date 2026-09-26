#pragma once

#include <cstdint>

namespace NR
{
	/**
	 * @brief Which creation-flag and proxy-selector pairing Feature 18 is driven with.
	 *        Both pairings receive the same sRGB-encoded display proxy; only the flags and the
	 *        Hdr/SDR selectors differ, so this is the one axis the proxy-contract A/B compares.
	 */
	enum class ProxyContract : uint32_t
	{
		kHdr = 0,  ///< IsHDR|DoSharpening|AutoExposure with DLSSNR.Hdr=1, SDR=0 (PR 723's pairing); the default.
		kSdr = 1,  ///< DoSharpening|AutoExposure with DLSSNR.Hdr=0, SDR=1, the A/B's other arm.
		kCount
	};

	/** @brief Pairings a persisted selector may name; the count clamps one that names none of them. */
	inline constexpr uint32_t kProxyContractCount = static_cast<uint32_t>(ProxyContract::kCount);

	/** @brief Clamps a persisted selector, so a hand-edited settings file cannot index past the enum. */
	[[nodiscard]] constexpr ProxyContract ClampProxyContract(uint32_t a_value)
	{
		return a_value < kProxyContractCount ? static_cast<ProxyContract>(a_value) : ProxyContract::kHdr;
	}

	/** @brief True when this pairing asks Feature 18 for its HDR route. */
	[[nodiscard]] constexpr bool ProxyIsHdr(ProxyContract a_contract)
	{
		return a_contract == ProxyContract::kHdr;
	}
}
