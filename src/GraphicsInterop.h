#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>
#include <cstdint>
#include <string>

class WrappedResource
{
public:
	// a_name follows Util::SetResourceName's "Feature::Name" convention; views get " SRV"/
	// " UAV"/" RTV" suffixes. Pass empty to leave resources unnamed.
	WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device, const std::string& a_name = {});
	~WrappedResource();

	ID3D11Texture2D* resource11 = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11UnorderedAccessView* uav = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	winrt::com_ptr<ID3D12Resource> resource;
};

/** @brief D3D12 fence shared into D3D11; value is the last value handed out by Next(). */
struct SharedFence
{
	static constexpr DWORD kRemovalPollMs = 100;
	/** @brief CPU bound for one fence value; a wedged GPU must not hang the render thread. */
	static constexpr DWORD kFenceTimeoutMs = 5000;
	winrt::com_ptr<ID3D12Fence> fence12;
	winrt::com_ptr<ID3D11Fence> fence11;
	uint64_t value = 0;

	/** @brief Returns the next value to signal, advancing the monotonic counter. */
	uint64_t Next() { return ++value; }
	/** @brief Releases both fence interfaces and returns the counter to zero. */
	void Reset()
	{
		fence12 = nullptr;
		fence11 = nullptr;
		value = 0;
	}
	/** @brief Outcome of a bounded CPU wait, so a caller can tell a slow GPU from a broken wait. */
	enum class WaitOutcome : uint8_t
	{
		kComplete,  ///< The fence reached the value.
		kTimeout,   ///< The fence did not reach the value within the bound.
		kFailed     ///< The wait itself failed: no event, no completion registration, or WAIT_FAILED.
	};

	/** @brief Creates and names the fence; throws on failure without leaking the NT handle. */
	void Create(ID3D12Device* a_device12, ID3D11Device5* a_device11, const char* a_name);
	/** @brief Waits on the CPU up to a_timeoutMs, polling device removal via fence12's own device.
	 *  Trivially kComplete when the fence is unset or a_value is 0 (nothing to wait for).
	 *  @param a_error Receives the error behind a kFailed outcome: the Win32 error from the wait,
	 *         or the HRESULT a failed completion registration returned; may be null. */
	WaitOutcome CpuWaitOutcome(uint64_t a_value, DWORD a_timeoutMs, DWORD* a_error = nullptr) const;
	/** @brief True only when the fence reached a_value inside the bound. */
	bool CpuWait(uint64_t a_value, DWORD a_timeoutMs) const { return CpuWaitOutcome(a_value, a_timeoutMs) == WaitOutcome::kComplete; }
};

