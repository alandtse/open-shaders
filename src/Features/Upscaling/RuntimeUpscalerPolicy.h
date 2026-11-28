#pragma once

namespace RuntimeUpscalerPolicy
{
	/** @brief Whether FSR dispatches through the D3D12 runtime provider instead of the D3D11 host SDK. */
	constexpr bool UseRuntimeProvider(bool a_providerPresent, bool a_amdAdapter, bool a_upgradeSelected)
	{
		return a_providerPresent && (a_amdAdapter || a_upgradeSelected);
	}

	/**
	 * @brief Whether the upgrade probe must release the D3D12 device it created.
	 * @param a_createdByProbe False when another consumer created the device first and still owns it.
	 * @param a_quarantined True after a provider fault, which leaves releasing its device unsafe.
	 */
	constexpr bool ReleaseProbeDevice(bool a_createdByProbe, bool a_quarantined, bool a_useRuntimeProvider)
	{
		return a_createdByProbe && !a_quarantined && !a_useRuntimeProvider;
	}
}
