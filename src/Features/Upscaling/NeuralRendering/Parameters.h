#pragma once

#include "ProxyContract.h"

#include <cstdint>

#include <nvsdk_ngx.h>

namespace NR
{
	// The SDK marks DoSharpening deprecated, but it belongs to the flag set the tested 310.8
	// runtime was driven with, so the deprecation is suppressed here alone.
#pragma warning(push)
#pragma warning(disable: 4996)
	/** @brief Creation flags of the kHdr pairing: IsHDR | DoSharpening | AutoExposure. */
	inline constexpr uint32_t kHdrProxyFlags =
		NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
		NVSDK_NGX_DLSS_Feature_Flags_DoSharpening |
		NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	/** @brief Creation flags of the kSdr pairing: DoSharpening | AutoExposure, without IsHDR. */
	inline constexpr uint32_t kSdrProxyFlags =
		NVSDK_NGX_DLSS_Feature_Flags_DoSharpening |
		NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
#pragma warning(pop)

	/** @brief Creation flags for a pairing; the value a create failure reports back. */
	[[nodiscard]] constexpr uint32_t ProxyFlags(ProxyContract a_contract)
	{
		return ProxyIsHdr(a_contract) ? kHdrProxyFlags : kSdrProxyFlags;
	}

	static_assert(ProxyFlags(ProxyContract::kHdr) == kHdrProxyFlags);
	static_assert(ProxyFlags(ProxyContract::kSdr) == kSdrProxyFlags);

	/**
	 * @brief Writes an integer-typed parameter into a driver-allocated NGX block.
	 *        int is the tag the tested 310.8 runtime was driven with for every integer key.
	 */
	inline void SetInt(NVSDK_NGX_Parameter* a_parameters, const char* a_key, uint32_t a_value)
	{
		a_parameters->Set(a_key, static_cast<int>(a_value));
	}

	/** @brief Float-typed write; float is the tag the tested 310.8 runtime was driven with for scalars. */
	inline void SetFloat(NVSDK_NGX_Parameter* a_parameters, const char* a_key, float a_value)
	{
		a_parameters->Set(a_key, a_value);
	}

	/**
	 * @brief Resource and matrix-typed write, always through the void* overload.
	 *        void* is the tag the tested 310.8 runtime was driven with for resources and matrices.
	 */
	inline void SetPointer(NVSDK_NGX_Parameter* a_parameters, const char* a_key, void* a_value)
	{
		a_parameters->Set(a_key, a_value);
	}
}
