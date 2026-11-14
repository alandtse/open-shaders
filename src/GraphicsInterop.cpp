#include "GraphicsInterop.h"
#include "Utils/D3D.h"

WrappedResource::WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device, const std::string& a_name)
{
	// Create D3D11 shared texture directly instead of wrapping D3D12 resource
	a_texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	auto throwIfFailed = [&](HRESULT a_result, const char* a_operation) {
		if (FAILED(a_result)) {
			logger::error(
				"[DX12SwapChain] Wrapped resource '{}' {} failed: HRESULT 0x{:08X}, dimensions {}x{}, format {}, bind flags 0x{:X}, misc flags 0x{:X}",
				a_name.empty() ? "<unnamed>" : a_name.c_str(),
				a_operation,
				static_cast<uint32_t>(a_result),
				a_texDesc.Width,
				a_texDesc.Height,
				static_cast<uint32_t>(a_texDesc.Format),
				a_texDesc.BindFlags,
				a_texDesc.MiscFlags);
		}
		DX::ThrowIfFailed(a_result);
	};

	// The raw members are assigned only at the end: a throw before those detaches would
	// leak them, since a partially constructed object runs no destructor.
	winrt::com_ptr<ID3D11Texture2D> texture11;
	throwIfFailed(a_d3d11Device->CreateTexture2D(&a_texDesc, nullptr, texture11.put()), "CreateTexture2D");
	if (!a_name.empty())
		Util::SetResourceName(texture11.get(), "%s", a_name.c_str());

	// Get shared handle from D3D11 texture to enable D3D12 access
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	throwIfFailed(texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())), "QueryInterface(IDXGIResource1)");
	winrt::handle sharedHandle;
	throwIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, sharedHandle.put()), "CreateSharedHandle");

	// Open the shared D3D11 texture as D3D12 resource
	winrt::com_ptr<ID3D12Resource> resource12;
	throwIfFailed(a_d3d12Device->OpenSharedHandle(sharedHandle.get(), IID_PPV_ARGS(resource12.put())), "OpenSharedHandle");
	if (!a_name.empty())
		resource12->SetName(winrt::to_hstring(a_name).c_str());

	winrt::com_ptr<ID3D11ShaderResourceView> srv11;
	if (a_texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = a_texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		throwIfFailed(a_d3d11Device->CreateShaderResourceView(texture11.get(), &srvDesc, srv11.put()), "CreateShaderResourceView");
		if (!a_name.empty())
			Util::SetResourceName(srv11.get(), "%s SRV", a_name.c_str());
	}

	winrt::com_ptr<ID3D11UnorderedAccessView> uav11;
	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		if (a_texDesc.ArraySize > 1) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = a_texDesc.ArraySize;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(texture11.get(), &uavDesc, uav11.put()), "CreateUnorderedAccessView");
		} else {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(texture11.get(), &uavDesc, uav11.put()), "CreateUnorderedAccessView");
		}
		if (!a_name.empty())
			Util::SetResourceName(uav11.get(), "%s UAV", a_name.c_str());
	}

	winrt::com_ptr<ID3D11RenderTargetView> rtv11;
	if (a_texDesc.BindFlags & D3D11_BIND_RENDER_TARGET) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = a_texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		throwIfFailed(a_d3d11Device->CreateRenderTargetView(texture11.get(), &rtvDesc, rtv11.put()), "CreateRenderTargetView");
		if (!a_name.empty())
			Util::SetResourceName(rtv11.get(), "%s RTV", a_name.c_str());
	}

	resource11 = texture11.detach();
	srv = srv11.detach();
	uav = uav11.detach();
	rtv = rtv11.detach();
	resource = std::move(resource12);
}

WrappedResource::~WrappedResource()
{
	if (resource11) {
		resource11->Release();
		resource11 = nullptr;
	}
	if (srv) {
		srv->Release();
		srv = nullptr;
	}
	if (uav) {
		uav->Release();
		uav = nullptr;
	}
	if (rtv) {
		rtv->Release();
		rtv = nullptr;
	}
	// resource (winrt::com_ptr) will be automatically released
}

void SharedFence::Create(ID3D12Device* a_device12, ID3D11Device5* a_device11, const char* a_name)
{
	DX::ThrowIfFailed(a_device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12)));
	winrt::handle sharedHandle;
	DX::ThrowIfFailed(a_device12->CreateSharedHandle(fence12.get(), nullptr, GENERIC_ALL, nullptr, sharedHandle.put()));
	DX::ThrowIfFailed(a_device11->OpenSharedFence(sharedHandle.get(), IID_PPV_ARGS(&fence11)));

	fence12->SetName(winrt::to_hstring(a_name).c_str());
	Util::SetResourceName(fence11.get(), "%s", a_name);
}

SharedFence::WaitOutcome SharedFence::CpuWaitOutcome(uint64_t a_value, DWORD a_timeoutMs, DWORD* a_error) const
{
	const auto failed = [a_error](DWORD a_lastError) {
		if (a_error)
			*a_error = a_lastError;
		return WaitOutcome::kFailed;
	};
	if (!fence12 || a_value == 0)
		return WaitOutcome::kComplete;
	if (fence12->GetCompletedValue() >= a_value)
		return WaitOutcome::kComplete;

	winrt::handle fenceEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
	if (!fenceEvent)
		return failed(GetLastError());
	if (const HRESULT registration = fence12->SetEventOnCompletion(a_value, fenceEvent.get()); FAILED(registration))
		return failed(static_cast<DWORD>(registration));

	winrt::com_ptr<ID3D12Device> device12;
	DWORD waitedMs = 0;
	while (waitedMs < a_timeoutMs) {
		const DWORD sliceMs = std::min<DWORD>(kRemovalPollMs, a_timeoutMs - waitedMs);
		const DWORD waitResult = WaitForSingleObject(fenceEvent.get(), sliceMs);
		if (waitResult == WAIT_OBJECT_0)
			return WaitOutcome::kComplete;
		if (waitResult != WAIT_TIMEOUT)
			return failed(GetLastError());
		waitedMs += sliceMs;
		if (!device12)
			fence12->GetDevice(IID_PPV_ARGS(&device12));
		if (device12 && FAILED(device12->GetDeviceRemovedReason()))
			return WaitOutcome::kFailed;
	}

	return WaitOutcome::kTimeout;
}
