#pragma once

#include <d3d12.h>
#include <ffx_antilag2_dx12.h>
#include <memory>
#include <utility>

namespace CS::AntiLag2
{
	using ContextPtr = std::unique_ptr<AMD::AntiLag2DX12::Context>;

	/** @brief Retains a supported SDK context and releases any interface returned by a failed initialization. */
	template <class InitializeFn>
	HRESULT InitializeContext(ContextPtr& context, ID3D12Device* device, InitializeFn initialize)
	{
		auto candidate = std::make_unique<AMD::AntiLag2DX12::Context>();
		const HRESULT hr = initialize(candidate.get(), device);
		if (hr == S_OK)
			context = std::move(candidate);
		else if (candidate->m_pAntiLagAPI)
			AMD::AntiLag2DX12::DeInitialize(candidate.get());
		return hr;
	}

	/** @brief Installs input interception only for a supported context, releasing it if attachment fails. */
	template <class InstallFn>
	bool InstallInputHook(ContextPtr& context, InstallFn install)
	{
		if (!context)
			return false;
		if (install())
			return true;
		AMD::AntiLag2DX12::DeInitialize(context.get());
		context.reset();
		return false;
	}

	/** @brief Drains presentation before disabling and releasing the context, retaining it on drain failure. */
	template <class DrainFn, class DisableFn>
	bool Shutdown(ContextPtr& context, DrainFn drain, DisableFn disable)
	{
		if (!context)
			return true;
		if (!drain()) {
			(void)context.release();
			return false;
		}
		disable();
		AMD::AntiLag2DX12::DeInitialize(context.get());
		context.reset();
		return true;
	}

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
