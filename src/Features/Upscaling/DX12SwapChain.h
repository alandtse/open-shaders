#pragma once

#include <Windows.Foundation.h>
#include <stdio.h>
#include <winrt/base.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

#include "antilag2sdk.h"
#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>
#include <memory>
#include <string>

#include <directx/d3dx12.h>

#include "GraphicsInterop.h"

struct DXGISwapChainProxy : IDXGISwapChain
{
public:
	DXGISwapChainProxy(IDXGISwapChain4* a_swapChain);

	IDXGISwapChain4* swapChain;

	/****IUnknown****/
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
	virtual ULONG STDMETHODCALLTYPE AddRef() override;
	virtual ULONG STDMETHODCALLTYPE Release() override;

	/****IDXGIObject****/
	virtual HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown) override;
	virtual HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent) override;

	/****IDXGIDeviceSubObject****/
	virtual HRESULT STDMETHODCALLTYPE GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice) override;

	/****IDXGISwapChain****/
	virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface) override;
	virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget) override;
	virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget) override;
	virtual HRESULT STDMETHODCALLTYPE GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc) override;
	virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
	virtual HRESULT STDMETHODCALLTYPE ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters) override;
	virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput) override;
	virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats) override;
	virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(_Out_ UINT* pLastPresentCount) override;
};

class DX12SwapChain
{
public:
	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;

	// 4x DLSS-G generates 3 interpolated frames per real frame; the SL pacer needs those
	// plus 2 slots of its own slack to avoid oversubscribing the flip-model chain.
	static constexpr UINT kMaxBackBuffers = 5;

	winrt::com_ptr<ID3D12CommandAllocator> commandAllocators[kMaxBackBuffers];
	winrt::com_ptr<ID3D12GraphicsCommandList4> commandLists[kMaxBackBuffers];

	IDXGISwapChain4* swapChain;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;

	WrappedResource* swapChainBufferWrapped;
	WrappedResource* uiBufferWrapped;

	// D3D12 interop resources for frame generation
	WrappedResource* depthBufferShared12 = nullptr;
	WrappedResource* motionVectorBufferShared12 = nullptr;

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	SharedFence interopFence;

	winrt::com_ptr<ID3D12Resource> swapChainBuffers[kMaxBackBuffers];

	UINT frameIndex = 0;

	UINT64 frameFenceValues[kMaxBackBuffers] = {};
	/** @brief CPU bound for the previous frame's allocator fence; a wedged GPU must not hang Present. */
	static constexpr DWORD kFrameFenceTimeoutMs = 1000;

	LARGE_INTEGER qpf;

	double refreshRate = 0;

	DXGISwapChainProxy* swapChainProxy = nullptr;

	bool useDLSSG = false;

	// Actual buffer count the live swap chain was created/resized with (<= kMaxBackBuffers).
	UINT backBufferCount = 2;

	// Returns the current frame time (in seconds) for accurate FPS calculation when frame generation is active
	float GetFrameTime() const;

	/** @brief Releases the Anti-Lag driver context before the D3D12 device. */
	~DX12SwapChain();
	/** @brief Updates the installed Anti-Lag session before input with FPS limiting disabled. */
	void UpdateAntiLag2BeforeInput();
	/** @brief Reports whether the installed session's driver state has Anti-Lag enabled. */
	bool IsAntiLag2Active() const { return antiLag2Context && antiLag2Enabled; }

	void CreateD3D12Device(IDXGIAdapter* a_adapter);
	void CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateSwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);

	void CreateInterop();
	/** @brief (Re)creates the D3D11/D3D12-shared swap-chain and UI buffer textures at (re)size time. */
	void RecreateWrappedResources(const DXGI_SWAP_CHAIN_DESC1& desc);

	DXGISwapChainProxy* GetSwapChainProxy();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	/** @brief IDXGISwapChain::GetBuffer equivalent for the wrapped D3D11 swap-chain buffer. Only buffer index 0 is supported. */
	HRESULT GetBuffer(UINT buffer, REFIID riid, void** ppSurface);
	/** @brief IDXGISwapChain::ResizeBuffers equivalent; rejects any bufferCount differing from the chain's own backBufferCount. */
	HRESULT ResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags);
	HRESULT Present(UINT SyncInterval, UINT Flags);
	/** @brief Presents through the session-enabled Anti-Lag path. */
	HRESULT PresentWithAntiLag2(UINT SyncInterval, UINT Flags);
	HRESULT GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice);
	HANDLE GetFrameLatencyWaitableObject();

	void SetColorSpace(bool enableHDR);

	// Resources needed by BackgroundBlur when D3D12 swap chain is active
	struct BlurResources
	{
		ID3D11Texture2D* backbufferTex = nullptr;
		ID3D11RenderTargetView* backbufferRTV = nullptr;
		ID3D11ShaderResourceView* backbufferSRV = nullptr;
		ID3D11ShaderResourceView* uiBufferSRV = nullptr;
		ID3D11RenderTargetView* uiBufferRTV = nullptr;
	};

	// Get all resources needed for background blur in one call
	BlurResources GetBlurResources() const;

	// D3D12 interop resource management
	void CreateSharedResources();

private:
	std::unique_ptr<AMD::AntiLag2DX12::Context> antiLag2Context;
	bool antiLag2Enabled = false;
	bool antiLag2UpdateFailureLogged = false;
	bool antiLag2MarkFailureLogged = false;
	bool antiLag2PrivateDataFailureLogged = false;

	/** @brief Initializes the Anti-Lag driver interface on the presentation device. */
	void InitializeAntiLag2();
	/** @brief Shares presentation logic while compiling Anti-Lag calls out of the ordinary path. */
	template <bool WithAntiLag2>
	HRESULT PresentImpl(UINT SyncInterval, UINT Flags);
	/** @brief Publishes the Anti-Lag context required by the FSR swap chain. */
	void PublishAntiLag2SwapChainData();
	/** @brief Signals the end of rendering to Anti-Lag before presentation. */
	void MarkAntiLag2EndOfFrame();
};
