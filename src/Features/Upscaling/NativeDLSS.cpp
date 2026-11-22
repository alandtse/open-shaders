#include "NativeDLSS.h"

#include <algorithm>
#include <cstring>
#include <directx/d3dx12.h>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include "../../GpuPass.h"
#include "../Upscaling.h"

bool NativeDLSS::Upscale(ID3D11Resource* a_colorOut, ID3D11Resource* a_motionVectors, ID3D11Resource* a_reactiveMask,
	ID3D11Resource* a_transparencyMask, const sl::Extent& a_extentIn, const sl::Extent& a_extentOut)
{
	const bool resumed = !active;
	active = false;

	auto& upscaling = globals::features::upscaling;
	auto* state = globals::state;
	const auto composite = upscaling.nativeCompositeFrame;
	auto* typedDepth = upscaling.typedDepthTexture;
	if (!failure.empty() || globals::game::isVR || !state || !typedDepth || !a_colorOut || !a_motionVectors ||
		!a_reactiveMask || !a_transparencyMask || !composite || !composite->valid || !composite->color ||
		!composite->producerFence || !composite->producerValue || composite->frame != state->frameCount ||
		composite->width != a_extentIn.width || composite->height != a_extentIn.height)
		return false;

	if (!EnsureStreamline() || !EnsureCommandContexts())
		return false;

	auto& swapChain = upscaling.dx12SwapChain;
	auto& streamline = upscaling.streamlineDX12;
	auto* context11 = swapChain.d3d11Context.get();
	auto* queue = swapChain.commandQueue.get();

	CS_GPU_PASS("Upscaling::NativeDLSS");

	CommandContext* commandContext = nullptr;
	bool retained = false;
	bool submitted = false;
	const auto abandon = [&](std::string_view a_reason) {
		// A retained composite or a submitted list must see a signal past it, or its slot and allocator stay busy.
		if ((retained || submitted) && !(commandContext && commandContext->fenceValue)) {
			try {
				const uint64_t rescue = fence.Next();
				DX::ThrowIfFailed(queue->Signal(fence.fence12.get(), rescue));
				if (submitted && commandContext)
					commandContext->fenceValue = rescue;
				lastSubmitted = rescue;
			} catch (...) {
			}
		}
		Latch(std::format("the DX12 DLSS dispatch threw: {}", a_reason));
		return false;
	};
	try {
		const std::array<ID3D11Resource*, kSharedCount> sources{ typedDepth->resource.get(), a_motionVectors, a_reactiveMask, a_transparencyMask, a_colorOut };
		if (!EnsureSharedResources(sources))
			return false;

		commandContext = AcquireCommandContext();
		if (!commandContext)
			return false;

		const FeatureKey key{ upscaling.settings.qualityMode, upscaling.settings.presetDLSS, a_extentOut.width, a_extentOut.height };
		if (featureKey && *featureKey != key) {
			// Streamline releases a replaced DLSS feature only from its present hook, which this instance lacks outside DLSS-G.
			if (!WaitForSubmitted())
				return false;
			streamline.DestroyDLSSResources();
		}
		featureKey = key;

		const D3D11_BOX inputBox{ 0, 0, 0, a_extentIn.width, a_extentIn.height, 1 };
		for (uint32_t i = 0; i < kOutput; ++i)
			context11->CopySubresourceRegion(shared[i]->resource11, 0, 0, 0, 0, sources[i], 0, &inputBox);
		const uint64_t inputsCopied = fence.Next();
		DX::ThrowIfFailed(context11->Signal(fence.fence11.get(), inputsCopied));
		DX::ThrowIfFailed(queue->Wait(fence.fence12.get(), inputsCopied));
		DX::ThrowIfFailed(queue->Wait(composite->producerFence.get(), composite->producerValue));

		const uint64_t done = fence.Next();
		if (!upscaling.nativeComposite.Retain(composite, fence.fence12.get(), done))
			return false;
		retained = true;

		auto* commandList = commandContext->commandList.get();
		DX::ThrowIfFailed(commandContext->allocator->Reset());
		DX::ThrowIfFailed(commandList->Reset(commandContext->allocator.get(), nullptr));

		constexpr size_t kInputCount = kOutput + 1;
		const std::array<ID3D12Resource*, kInputCount> inputs{ composite->color.get(), shared[kDepth]->resource.get(),
			shared[kMotionVectors]->resource.get(), shared[kReactiveMask]->resource.get(), shared[kTransparencyMask]->resource.get() };
		auto* output = shared[kOutput]->resource.get();
		const auto transition = [&](bool a_toDLSS) {
			std::array<D3D12_RESOURCE_BARRIER, kInputCount + 1> barriers{};
			for (size_t i = 0; i < inputs.size(); ++i)
				barriers[i] = a_toDLSS ?
				                  CD3DX12_RESOURCE_BARRIER::Transition(inputs[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) :
				                  CD3DX12_RESOURCE_BARRIER::Transition(inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			barriers.back() = a_toDLSS ?
			                      CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) :
			                      CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		};
		transition(true);
		streamline.pendingHistoryReset |= resumed;
		const bool evaluated = streamline.EvaluateDLSSDX12(commandList, inputs[0], output, inputs[1], inputs[2], inputs[3], inputs[4], a_extentIn, a_extentOut);
		transition(false);
		DX::ThrowIfFailed(commandList->Close());

		ID3D12CommandList* lists[]{ commandList };
		queue->ExecuteCommandLists(1, lists);
		submitted = true;
		DX::ThrowIfFailed(queue->Signal(fence.fence12.get(), done));
		commandContext->fenceValue = done;
		lastSubmitted = done;
		DX::ThrowIfFailed(context11->Wait(fence.fence11.get(), done));

		if (!evaluated) {
			Latch("the DX12 DLSS evaluate failed");
			return false;
		}

		const D3D11_BOX outputBox{ 0, 0, 0, a_extentOut.width, a_extentOut.height, 1 };
		context11->CopySubresourceRegion(a_colorOut, 0, 0, 0, 0, shared[kOutput]->resource11, 0, &outputBox);
		active = true;
		return true;
	} catch (const std::exception& e) {
		return abandon(e.what());
	} catch (...) {
		return abandon("an unknown exception");
	}
}

NativeDLSS::~NativeDLSS()
{
	const uint64_t completed = fence.fence12 ? fence.fence12->GetCompletedValue() : UINT64_MAX;
	if (completed != UINT64_MAX && completed < lastSubmitted) {
		ParkInFlight();
		for (auto& commandContext : commandContexts) {
			(void)commandContext.allocator.detach();
			(void)commandContext.commandList.detach();
		}
	}
}

void NativeDLSS::Reset()
{
	active = false;
	if (!featureKey && !shared[kOutput])
		return;

	if (WaitForSubmitted()) {
		globals::features::upscaling.streamlineDX12.DestroyDLSSResources();
		for (auto& resource : shared)
			resource.reset();
	} else {
		ParkInFlight();
	}
	sharedDescs = {};
	featureKey.reset();
}

void NativeDLSS::ParkInFlight()
{
	static auto* pending = new std::vector<std::unique_ptr<WrappedResource>>;
	for (auto& resource : shared)
		if (resource)
			pending->push_back(std::move(resource));
}

bool NativeDLSS::EnsureStreamline()
{
	auto& upscaling = globals::features::upscaling;
	auto& streamline = upscaling.streamlineDX12;
	auto& swapChain = upscaling.dx12SwapChain;
	if (!streamline.initialized || !swapChain.EnsureSharedDevice())
		return false;

	if (!streamline.d3d12DeviceBound) {
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		winrt::com_ptr<IDXGIAdapter> adapter;
		if (FAILED(swapChain.d3d11Device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))) || FAILED(dxgiDevice->GetAdapter(adapter.put()))) {
			Latch("the D3D11 device's adapter could not be read");
			return false;
		}
		streamline.SetD3DDevice12(swapChain.d3d12Device.get());
		if (!streamline.d3d12DeviceBound) {
			Latch("the DX12 Streamline instance could not bind the D3D12 device");
			return false;
		}
		streamline.CheckFeatures(adapter.get());
		streamline.PostDevice();
	}

	if (!streamline.featureDLSS || !streamline.slDLSSSetOptions || !streamline.slEvaluateFeature) {
		Latch("DLSS is not available on the DX12 Streamline instance");
		return false;
	}
	return true;
}

bool NativeDLSS::EnsureCommandContexts()
{
	if (fence.fence12 && commandContexts.back().commandList)
		return true;

	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	auto* device = swapChain.d3d12Device.get();
	try {
		fence.Create(device, swapChain.d3d11Device.get(), "NativeDLSS::Fence");
		for (auto& commandContext : commandContexts) {
			DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandContext.allocator.put())));
			DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandContext.allocator.get(), nullptr, IID_PPV_ARGS(commandContext.commandList.put())));
			DX::ThrowIfFailed(commandContext.commandList->Close());
		}
	} catch (const std::exception& e) {
		Latch(std::format("the DX12 DLSS command lists could not be created: {}", e.what()));
		return false;
	} catch (...) {
		Latch("the DX12 DLSS command lists could not be created");
		return false;
	}
	return true;
}

bool NativeDLSS::EnsureSharedResources(const std::array<ID3D11Resource*, kSharedCount>& a_sources)
{
	std::array<D3D11_TEXTURE2D_DESC, kSharedCount> descs{};
	for (uint32_t i = 0; i < kSharedCount; ++i) {
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(a_sources[i]->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;
		auto& desc = descs[i];
		texture->GetDesc(&desc);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = i == kOutput ? D3D11_BIND_UNORDERED_ACCESS : 0;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
	}

	if (std::ranges::all_of(shared, [](const auto& a_resource) { return a_resource != nullptr; }) &&
		std::memcmp(descs.data(), sharedDescs.data(), sizeof(descs)) == 0)
		return true;

	if (!WaitForSubmitted())
		return false;
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	static constexpr std::array<const char*, kSharedCount> kNames{ "NativeDLSS::Depth", "NativeDLSS::MotionVectors",
		"NativeDLSS::ReactiveMask", "NativeDLSS::TransparencyMask", "NativeDLSS::Output" };
	for (uint32_t i = 0; i < kSharedCount; ++i)
		shared[i] = std::make_unique<WrappedResource>(descs[i], swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), kNames[i]);
	sharedDescs = descs;
	return true;
}

NativeDLSS::CommandContext* NativeDLSS::AcquireCommandContext()
{
	auto& oldest = *std::ranges::min_element(commandContexts, {}, &CommandContext::fenceValue);
	if (!fence.CpuWait(oldest.fenceValue, SharedFence::kFenceTimeoutMs))
		return nullptr;
	oldest.fenceValue = 0;
	return &oldest;
}

bool NativeDLSS::WaitForSubmitted()
{
	return fence.CpuWait(lastSubmitted, SharedFence::kFenceTimeoutMs);
}

void NativeDLSS::Latch(std::string a_reason)
{
	if (!failure.empty())
		return;
	failure = std::move(a_reason);
	logger::error("[Upscaling] DX12 DLSS is off for this session and DX11 DLSS takes over: {}", failure);
}
