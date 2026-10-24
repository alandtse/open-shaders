#pragma once

#include <d3d12.h>
#include <ffx_antilag2_dx12.h>

namespace CS::AntiLag2
{
	/** @brief Applies driver state transactionally before invoking the SDK's pre-input delay. */
	inline HRESULT Update(AMD::AntiLag2DX12::Context* context, bool enabled)
	{
		constexpr unsigned int maxFPS = 0;
		if (!context || !context->m_pAntiLagAPI)
			return E_NOINTERFACE;
		if (context->m_enabled != enabled || context->m_maxFPS != maxFPS) {
			AMD::AntiLag2DX12::APIData_v1 data{};
			data.uiSize = sizeof(data);
			data.uiVersion = 1;
			data.eMode = enabled ? 1 : 2;
			data.maxFPS = maxFPS;
			const HRESULT hr = context->m_pAntiLagAPI->UpdateAntiLagState(&data);
			if (FAILED(hr))
				return hr;
			// Upstream caches state before checking the driver, so synchronize only after success to keep failed changes retryable.
			context->m_enabled = enabled;
			context->m_maxFPS = maxFPS;
		}
		return AMD::AntiLag2DX12::Update(context, enabled, maxFPS);
	}
}
