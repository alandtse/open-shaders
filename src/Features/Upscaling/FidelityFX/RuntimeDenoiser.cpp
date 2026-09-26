#include "../../../Globals.h"
#include "../../../Util.h"
#include "../../Upscaling.h"
#include "../DX12SwapChain.h"
#include "../FidelityFX.h"
#include "RadianceCacheCompat.h"

#include <directx/d3dx12.h>

extern ffxFunctions ffxModule;

namespace
{
	D3D11_TEXTURE2D_DESC SharedDesc(ID3D11Texture2D* source, uint32_t width, uint32_t height, UINT bind)
	{
		D3D11_TEXTURE2D_DESC d{};
		source->GetDesc(&d);
		d.Width = width;
		d.Height = height;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.SampleDesc = { 1, 0 };
		d.Usage = D3D11_USAGE_DEFAULT;
		d.CPUAccessFlags = 0;
		d.BindFlags = bind;
		d.MiscFlags = 0;
		return d;
	}

	void DeleteWrapped(WrappedResource*& p)
	{
		delete p;
		p = nullptr;
	}

	FfxApiMatrix4x4 ToFfxMatrix(const Matrix& source)
	{
		// Skyrim's frame CB is consumed by HLSL; transpose to the row-major/row-vector
		// convention explicitly required by Denoiser 1.2.
		const auto m = source.Transpose();
		FfxApiMatrix4x4 out{};
		static_assert(sizeof(out) == sizeof(m));
		std::memcpy(&out, &m, sizeof(out));
		return out;
	}

	ffxReturnCode_t DispatchDenoiserProtected(ffxContext* context, const ffxDispatchDescHeader* desc, bool& faulted)
	{
		// Keep SEH in a leaf function with no C++ objects requiring unwinding; MSVC
		// rejects __try in functions that need normal C++ stack unwinding (C2712).
		faulted = false;
		ffxReturnCode_t result = FFX_API_RETURN_ERROR;
		__try {
			result = ffxModule.Dispatch(context, desc);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			faulted = true;
		}
		return result;
	}
}

namespace
{
	bool CreateRadianceCacheBuffer(
		ID3D12Device* device,
		uint64_t elementCount,
		uint32_t stride,
		const wchar_t* name,
		winrt::com_ptr<ID3D12Resource>& resource)
	{
		if (!device || !elementCount || !stride)
			return false;

		const uint64_t size = elementCount * static_cast<uint64_t>(stride);

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Alignment = 0;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		const HRESULT hr = device->CreateCommittedResource(
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(resource.put()));

		if (FAILED(hr))
			return false;

		if (name)
			resource->SetName(name);

		return true;
	}
}
namespace
{
	bool CreateRadianceCacheUploadBuffer(
		ID3D12Device* device,
		winrt::com_ptr<ID3D12Resource>& resource)
	{
		if (!device)
			return false;

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = sizeof(uint32_t) * 2;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		const HRESULT hr = device->CreateCommittedResource(
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(resource.put()));

		if (FAILED(hr))
			return false;

		resource->SetName(L"RadianceCache::CounterUpload");

		uint32_t* mapped = nullptr;
		D3D12_RANGE readRange{ 0, 0 };

		if (FAILED(resource->Map(0, &readRange, reinterpret_cast<void**>(&mapped))))
			return false;

		mapped[0] = 0;
		mapped[1] = 0;

		D3D12_RANGE writtenRange{ 0, sizeof(uint32_t) * 2 };
		resource->Unmap(0, &writtenRange);

		return true;
	}
}
namespace
{
	bool CreateRadianceCacheStagingBuffer(
		ID3D12Device* device,
		uint64_t size,
		D3D12_HEAP_TYPE heapType,
		D3D12_RESOURCE_STATES initialState,
		const wchar_t* name,
		winrt::com_ptr<ID3D12Resource>& resource)
	{
		if (!device || size == 0)
			return false;

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = heapType;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		const HRESULT hr = device->CreateCommittedResource(
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			initialState,
			nullptr,
			IID_PPV_ARGS(resource.put()));

		if (FAILED(hr))
			return false;

		if (name)
			resource->SetName(name);

		return true;
	}
}
bool FidelityFX::EnsureRadianceCacheBuffers()
{
	if (radianceCacheBuffersReady)
		return true;

	if (!EnsureRadianceCacheContext())
		return false;

	auto& sc = globals::features::upscaling.dx12SwapChain;
	if (!sc.d3d12Device)
		return false;

	try {
		if (!CreateRadianceCacheBuffer(
				sc.d3d12Device.get(),
				radianceCacheInferenceCapacity,
				sizeof(RadianceCacheInput),
				L"RadianceCache::PredictionInputs",
				radianceCachePredictionInputs))
			throw std::runtime_error("prediction input allocation failed");

		if (!CreateRadianceCacheBuffer(
				sc.d3d12Device.get(),
				radianceCacheInferenceCapacity,
				sizeof(RadianceCacheOutput),
				L"RadianceCache::PredictionOutputs",
				radianceCachePredictionOutputs))
			throw std::runtime_error("prediction output allocation failed");

		if (!CreateRadianceCacheBuffer(
				sc.d3d12Device.get(),
				radianceCacheTrainingCapacity,
				sizeof(RadianceCacheInput),
				L"RadianceCache::TrainInputs",
				radianceCacheTrainInputs))
			throw std::runtime_error("training input allocation failed");

		if (!CreateRadianceCacheBuffer(
				sc.d3d12Device.get(),
				radianceCacheTrainingCapacity,
				sizeof(RadianceCacheOutput),
				L"RadianceCache::TrainTargets",
				radianceCacheTrainTargets))
			throw std::runtime_error("training target allocation failed");

		if (!CreateRadianceCacheBuffer(
				sc.d3d12Device.get(),
				2,
				sizeof(uint32_t),
				L"RadianceCache::SampleCounters",
				radianceCacheSampleCounters))
			throw std::runtime_error("sample counter allocation failed");

		if (!CreateRadianceCacheUploadBuffer(
				sc.d3d12Device.get(),
				radianceCacheCounterUpload))
			throw std::runtime_error("counter upload allocation failed");
		if (!CreateRadianceCacheStagingBuffer(
				sc.d3d12Device.get(),
				sizeof(RadianceCacheInput) * 4096,
				D3D12_HEAP_TYPE_UPLOAD,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				L"RadianceCache::InputUpload",
				radianceCacheInputUpload))
			throw std::runtime_error("Radiance Cache input upload allocation failed");

		if (!CreateRadianceCacheStagingBuffer(
				sc.d3d12Device.get(),
				sizeof(RadianceCacheInput) * 4096,
				D3D12_HEAP_TYPE_UPLOAD,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				L"RadianceCache::TrainingInputUpload",
				radianceCacheTrainingInputUpload))
			throw std::runtime_error("Radiance Cache training input upload allocation failed");

		if (!CreateRadianceCacheStagingBuffer(
				sc.d3d12Device.get(),
				sizeof(RadianceCacheOutput) * 4096,
				D3D12_HEAP_TYPE_UPLOAD,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				L"RadianceCache::TrainingTargetUpload",
				radianceCacheTrainingTargetUpload))
			throw std::runtime_error("Radiance Cache training target upload allocation failed");

		if (!CreateRadianceCacheStagingBuffer(
				sc.d3d12Device.get(),
				sizeof(RadianceCacheOutput) * 4096,
				D3D12_HEAP_TYPE_READBACK,
				D3D12_RESOURCE_STATE_COPY_DEST,
				L"RadianceCache::OutputReadback",
				radianceCacheOutputReadback))
			throw std::runtime_error("Radiance Cache output readback allocation failed");
		if (!CreateRadianceCacheStagingBuffer(
				sc.d3d12Device.get(),
				sizeof(RadianceCacheInput) * 4096,
				D3D12_HEAP_TYPE_READBACK,
				D3D12_RESOURCE_STATE_COPY_DEST,
				L"RadianceCache::InputReadback",
				radianceCacheInputReadback))
			throw std::runtime_error("Radiance Cache input readback allocation failed");
		radianceCacheBuffersReady = true;

		return true;
	} catch (const std::exception& e) {
		logger::error(
			"[RadianceCache] buffer creation failed: {}",
			e.what());

		radianceCachePredictionInputs = nullptr;
		radianceCachePredictionOutputs = nullptr;
		radianceCacheTrainInputs = nullptr;
		radianceCacheTrainTargets = nullptr;
		radianceCacheSampleCounters = nullptr;
		radianceCacheCounterUpload = nullptr;
		radianceCacheInputUpload = nullptr;
		radianceCacheTrainingInputUpload = nullptr;
		radianceCacheTrainingTargetUpload = nullptr;
		radianceCacheInputReadback = nullptr;
		radianceCacheOutputReadback = nullptr;
		radianceCacheBuffersReady = false;

		return false;
	}
}
bool FidelityFX::DispatchRadianceCacheCapturedInference(
	const float* a_inputValues,
	const uint32_t* a_sourceQueryIndices,
	uint32_t a_sampleCount,
	const float* a_trainingInputValues,
	const float* a_trainingTargetValues,
	uint32_t a_trainingSampleCount,
	float* a_outputRadiance,
	uint32_t* a_outputSourceQueryIndices,
	uint32_t* a_outputSampleCount,
	bool* a_outputReady)
{
	if (a_outputSampleCount)
		*a_outputSampleCount = 0;
	if (a_outputReady)
		*a_outputReady = false;

	// A zero-sized batch is a poll-only call. This lets the
	// caller collect an already submitted asynchronous inference even
	// when the current capture produced no valid NRC queries.
	const bool pollOnly = (a_sampleCount == 0);

	if (a_sampleCount > 4096 ||
		a_trainingSampleCount > radianceCacheTrainingCapacity ||
		(!pollOnly && (!a_inputValues || !a_sourceQueryIndices)) ||
		(a_trainingSampleCount > 0 &&
			(!a_trainingInputValues || !a_trainingTargetValues))) {
		logger::error(
			"[RadianceCache] invalid inference/training batch: inference={}, training={}",
			a_sampleCount,
			a_trainingSampleCount);
		return false;
	}

	if (!EnsureRadianceCacheBuffers())
		return false;

	if (!ffxModule.Dispatch || !radianceCacheContext || !radianceCacheInputUpload ||
		!radianceCacheTrainingInputUpload || !radianceCacheTrainingTargetUpload ||
		!radianceCacheOutputReadback || !runtimeD3D12Fence)
		return false;

	// Collect the previous inference only when the GPU has already completed it.
	// GetCompletedValue() is a poll; unlike WaitForRuntimeD3D12Fence(), it never
	// blocks the render thread.
	if (radianceCacheCapturedInferencePending) {
		const uint64_t completedFence = runtimeD3D12Fence->GetCompletedValue();

		if (completedFence < radianceCacheCapturedInferenceFence) {
			// One inference is already in flight. Do not overwrite the
			// single upload/readback set and, critically, do not wait.
			if (rrDiagnosticsEnabled)
				++radianceCacheCapturedInferencePollCount;
			return true;
		}

		const uint32_t completedSampleCount =
			radianceCacheCapturedInferenceSampleCount;
		const size_t completedOutputBytes =
			sizeof(RadianceCacheOutput) * static_cast<size_t>(completedSampleCount);

		std::vector<RadianceCacheOutput> outputs(completedSampleCount);

		void* mapped = nullptr;
		D3D12_RANGE readRange{ 0, completedOutputBytes };

		if (FAILED(radianceCacheOutputReadback->Map(0, &readRange, &mapped)) || !mapped) {
			logger::error("[RadianceCache] failed to map completed asynchronous inference output");
			radianceCacheCapturedInferencePending = false;
			return false;
		}

		std::memcpy(outputs.data(), mapped, completedOutputBytes);
		D3D12_RANGE writtenRange{ 0, 0 };
		radianceCacheOutputReadback->Unmap(0, &writtenRange);

		uint32_t finiteOutputCount = 0;
		for (const auto& output : outputs) {
			if (std::isfinite(output.radiance[0]) &&
				std::isfinite(output.radiance[1]) &&
				std::isfinite(output.radiance[2]))
				++finiteOutputCount;
		}

		if (finiteOutputCount != completedSampleCount) {
			logger::error(
				"[RadianceCache] NRC output validation failed: finite={}/{}",
				finiteOutputCount,
				completedSampleCount);
			radianceCacheCapturedInferencePending = false;
			return false;
		}

		if (a_outputRadiance) {
			for (uint32_t i = 0; i < completedSampleCount; ++i) {
				a_outputRadiance[i * 3 + 0] = outputs[i].radiance[0];
				a_outputRadiance[i * 3 + 1] = outputs[i].radiance[1];
				a_outputRadiance[i * 3 + 2] = outputs[i].radiance[2];
			}
		}

		if (a_outputSourceQueryIndices) {
			std::memcpy(
				a_outputSourceQueryIndices,
				radianceCacheCapturedInferenceSourceIndices,
				sizeof(uint32_t) * completedSampleCount);
		}

		if (a_outputSampleCount)
			*a_outputSampleCount = completedSampleCount;
		if (a_outputReady)
			*a_outputReady = true;

		radianceCacheCapturedInferencePending = false;
		radianceCacheCapturedInferenceFence = 0;
		radianceCacheCapturedInferenceSampleCount = 0;
	}

	// Poll-only service ends here. If a previous inference
	// completed, its result has already been returned above. Never
	// submit an empty NRC inference when this call had no new queries.
	if (pollOnly)
		return true;
	const size_t inputBytes = sizeof(RadianceCacheInput) * static_cast<size_t>(a_sampleCount);
	const size_t outputBytes = sizeof(RadianceCacheOutput) * static_cast<size_t>(a_sampleCount);
	const size_t trainingInputBytes =
		sizeof(RadianceCacheInput) * static_cast<size_t>(a_trainingSampleCount);
	const size_t trainingTargetBytes =
		sizeof(RadianceCacheOutput) * static_cast<size_t>(a_trainingSampleCount);

	{
		void* mapped = nullptr;
		D3D12_RANGE readRange{ 0, 0 };

		if (FAILED(radianceCacheInputUpload->Map(0, &readRange, &mapped)) || !mapped) {
			logger::error("[RadianceCache] failed to map D3D12 batch input upload buffer");
			return false;
		}

		std::memcpy(mapped, a_inputValues, inputBytes);
		D3D12_RANGE writtenRange{ 0, inputBytes };
		radianceCacheInputUpload->Unmap(0, &writtenRange);
	}

	if (a_trainingSampleCount > 0) {
		{
			void* mapped = nullptr;
			D3D12_RANGE readRange{ 0, 0 };

			if (FAILED(radianceCacheTrainingInputUpload->Map(0, &readRange, &mapped)) || !mapped) {
				logger::error("[RadianceCache] failed to map training input upload buffer");
				return false;
			}

			std::memcpy(mapped, a_trainingInputValues, trainingInputBytes);
			D3D12_RANGE writtenRange{ 0, trainingInputBytes };
			radianceCacheTrainingInputUpload->Unmap(0, &writtenRange);
		}

		{
			void* mapped = nullptr;
			D3D12_RANGE readRange{ 0, 0 };

			if (FAILED(radianceCacheTrainingTargetUpload->Map(0, &readRange, &mapped)) || !mapped) {
				logger::error("[RadianceCache] failed to map training target upload buffer");
				return false;
			}

			std::memcpy(mapped, a_trainingTargetValues, trainingTargetBytes);
			D3D12_RANGE writtenRange{ 0, trainingTargetBytes };
			radianceCacheTrainingTargetUpload->Unmap(0, &writtenRange);
		}
	}

	auto& sc = globals::features::upscaling.dx12SwapChain;
	if (!sc.commandQueue)
		return false;

	// Counter 0 = inference samples, counter 1 = training samples.
	{
		uint32_t counters[2] = { a_sampleCount, a_trainingSampleCount };

		void* mapped = nullptr;
		D3D12_RANGE readRange{ 0, 0 };

		if (FAILED(radianceCacheCounterUpload->Map(
				0,
				&readRange,
				&mapped))) {
			logger::error(
				"[RadianceCache] failed to map sample-counter upload buffer");
			return false;
		}

		std::memcpy(mapped, counters, sizeof(counters));

		D3D12_RANGE writtenRange{ 0, sizeof(counters) };
		radianceCacheCounterUpload->Unmap(0, &writtenRange);
	}

	auto* cc = AcquireRuntimeCommandContext();
	if (!cc)
		return false;

	bool commandListRecording = false;

	try {
		DX::ThrowIfFailed(cc->commandAllocator->Reset());
		DX::ThrowIfFailed(
			cc->commandList->Reset(
				cc->commandAllocator.get(),
				nullptr));

		commandListRecording = true;
		auto inputToCopy =
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCachePredictionInputs.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_COPY_DEST);

		auto countersToCopy =
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCacheSampleCounters.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_COPY_DEST);

		std::vector<D3D12_RESOURCE_BARRIER> copyBarriers;
		copyBarriers.push_back(inputToCopy);
		copyBarriers.push_back(countersToCopy);

		if (a_trainingSampleCount > 0) {
			copyBarriers.push_back(
				CD3DX12_RESOURCE_BARRIER::Transition(
					radianceCacheTrainInputs.get(),
					D3D12_RESOURCE_STATE_COMMON,
					D3D12_RESOURCE_STATE_COPY_DEST));

			copyBarriers.push_back(
				CD3DX12_RESOURCE_BARRIER::Transition(
					radianceCacheTrainTargets.get(),
					D3D12_RESOURCE_STATE_COMMON,
					D3D12_RESOURCE_STATE_COPY_DEST));
		}

		cc->commandList->ResourceBarrier(
			static_cast<UINT>(copyBarriers.size()),
			copyBarriers.data());

		cc->commandList->CopyBufferRegion(
			radianceCachePredictionInputs.get(),
			0,
			radianceCacheInputUpload.get(),
			0,
			inputBytes);

		if (a_trainingSampleCount > 0) {
			cc->commandList->CopyBufferRegion(
				radianceCacheTrainInputs.get(),
				0,
				radianceCacheTrainingInputUpload.get(),
				0,
				trainingInputBytes);

			cc->commandList->CopyBufferRegion(
				radianceCacheTrainTargets.get(),
				0,
				radianceCacheTrainingTargetUpload.get(),
				0,
				trainingTargetBytes);
		}

		cc->commandList->CopyBufferRegion(
			radianceCacheSampleCounters.get(),
			0,
			radianceCacheCounterUpload.get(),
			0,
			sizeof(uint32_t) * 2);

		std::vector<D3D12_RESOURCE_BARRIER> begin;
		begin.push_back(
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCachePredictionInputs.get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

		begin.push_back(
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCachePredictionOutputs.get(),
				D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		if (a_trainingSampleCount > 0) {
			begin.push_back(
				CD3DX12_RESOURCE_BARRIER::Transition(
					radianceCacheTrainInputs.get(),
					D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

			begin.push_back(
				CD3DX12_RESOURCE_BARRIER::Transition(
					radianceCacheTrainTargets.get(),
					D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}

		begin.push_back(
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCacheSampleCounters.get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		cc->commandList->ResourceBarrier(
			static_cast<UINT>(begin.size()),
			begin.data());

		ffxDispatchDescRadianceCache dispatch{};
		dispatch.header.type =
			FFX_API_DISPATCH_DESC_TYPE_RADIANCECACHE;

		dispatch.commandList = cc->commandList.get();

		dispatch.predictionInputs =
			ffxApiGetResourceDX12(
				radianceCachePredictionInputs.get(),
				FFX_API_RESOURCE_STATE_COMPUTE_READ);

		dispatch.predictionInputs.description.stride =
			sizeof(RadianceCacheInput);

		dispatch.predictionOutputs =
			ffxApiGetResourceDX12(
				radianceCachePredictionOutputs.get(),
				FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
				FFX_API_RESOURCE_USAGE_UAV);

		dispatch.predictionOutputs.description.stride =
			sizeof(RadianceCacheOutput);

		dispatch.trainInputs =
			ffxApiGetResourceDX12(
				radianceCacheTrainInputs.get(),
				FFX_API_RESOURCE_STATE_COMPUTE_READ);

		dispatch.trainInputs.description.stride =
			sizeof(RadianceCacheInput);

		dispatch.trainTargets =
			ffxApiGetResourceDX12(
				radianceCacheTrainTargets.get(),
				FFX_API_RESOURCE_STATE_COMPUTE_READ);

		dispatch.trainTargets.description.stride =
			sizeof(RadianceCacheOutput);

		dispatch.sampleCounters =
			ffxApiGetResourceDX12(
				radianceCacheSampleCounters.get(),
				FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
				FFX_API_RESOURCE_USAGE_UAV);

		dispatch.sampleCounters.description.stride =
			sizeof(uint32_t);
		// Keep the proven inference/counter behavior. If the snapped
		// world-space Radiance Cache volume changed, reset NRC in this same dispatch.
		const bool applyRadianceCacheReset =
			radianceCacheResetPending.load();

		dispatch.flags =
			FFX_RADIANCE_CACHE_DISPATCH_INFERENCE |
			FFX_RADIANCE_CACHE_CLEAR_ALL_COUNTERS |
			FFX_RADIANCE_CACHE_OVERRIDE_LEARNING_RATE |
			FFX_RADIANCE_CACHE_OVERRIDE_WEIGHT_SMOOTHING;

		dispatch.overrides.learningRate =
			radianceCacheLearningRate;
		dispatch.overrides.weightSmoothing =
			radianceCacheWeightSmoothing;

		if (a_trainingSampleCount > 0)
			dispatch.flags |= FFX_RADIANCE_CACHE_DISPATCH_TRAINING;

		if (applyRadianceCacheReset)
			dispatch.flags |= FFX_RADIANCE_CACHE_RESET;

		bool dispatchFaulted = false;

		const ffxReturnCode_t dispatchResult =
			DispatchDenoiserProtected(
				&radianceCacheContext,
				&dispatch.header,
				dispatchFaulted);

		if (dispatchFaulted ||
			dispatchResult != FFX_API_RETURN_OK) {
			radianceCacheFailureLatched = true;

			logger::error(
				"[RadianceCache] NRC batch dispatch {} failed (return code {})",
				dispatchFaulted ? "faulted inside provider" : "returned failure",
				static_cast<int>(dispatchResult));

			if (commandListRecording) {
				(void)cc->commandList->Close();
				commandListRecording = false;
			}

			return false;
		}

		auto outputToCopy =
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCachePredictionOutputs.get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COPY_SOURCE);

		cc->commandList->ResourceBarrier(
			1,
			&outputToCopy);

		cc->commandList->CopyBufferRegion(
			radianceCacheOutputReadback.get(),
			0,
			radianceCachePredictionOutputs.get(),
			0,
			outputBytes);

		std::vector<D3D12_RESOURCE_BARRIER> end;

		end.push_back(
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCachePredictionInputs.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COMMON));

		end.push_back(
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCachePredictionOutputs.get(),
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_COMMON));

		if (a_trainingSampleCount > 0) {
			end.push_back(
				CD3DX12_RESOURCE_BARRIER::Transition(
					radianceCacheTrainInputs.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_COMMON));

			end.push_back(
				CD3DX12_RESOURCE_BARRIER::Transition(
					radianceCacheTrainTargets.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_COMMON));
		}

		end.push_back(
			CD3DX12_RESOURCE_BARRIER::Transition(
				radianceCacheSampleCounters.get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COMMON));

		cc->commandList->ResourceBarrier(
			static_cast<UINT>(end.size()),
			end.data());

		DX::ThrowIfFailed(cc->commandList->Close());
		commandListRecording = false;

		ID3D12CommandList* lists[] = {
			cc->commandList.get()
		};

		sc.commandQueue->ExecuteCommandLists(1, lists);

		const uint64_t fenceValue =
			runtimeFenceValue++;

		DX::ThrowIfFailed(
			sc.commandQueue->Signal(
				runtimeD3D12Fence.get(),
				fenceValue));

		cc->fenceValue = fenceValue;
		radianceCacheLastFenceValue = fenceValue;

		// Ownership of the single upload/readback set now belongs
		// to this fence until a later call observes GPU completion.
		radianceCacheCapturedInferencePending = true;
		radianceCacheCapturedInferenceFence = fenceValue;
		radianceCacheCapturedInferenceSampleCount = a_sampleCount;
		radianceCacheCapturedInferencePollCount = 0;
		std::memcpy(
			radianceCacheCapturedInferenceSourceIndices,
			a_sourceQueryIndices,
			sizeof(uint32_t) * a_sampleCount);

		// Consume the reset when the reset-bearing command list has been
		// successfully submitted. Waiting for CPU readback is no longer part
		// of dispatch completion.
		if (applyRadianceCacheReset) {
			radianceCacheResetPending.store(false);
		}

		return true;
	} catch (const std::exception& e) {
		logger::error(
			"[RadianceCache] NRC batch inference failed: {}",
			e.what());

		if (commandListRecording)
			(void)cc->commandList->Close();

		return false;
	}
}
void FidelityFX::RequestRadianceCacheReset()
{
	radianceCacheResetPending.store(true);
}

void FidelityFX::ResetRadianceCache()
{
	if (radianceCacheLastFenceValue != 0 && runtimeD3D12Fence) {
		if (!WaitForRuntimeD3D12Fence(radianceCacheLastFenceValue)) {
			logger::error(
				"[RadianceCache] Timed out waiting for the last Radiance Cache dispatch; preserving context/resources");
			radianceCacheFailureLatched = true;
			return;
		}
	}

	if (radianceCacheContext && ffxModule.DestroyContext)
		(void)ffxModule.DestroyContext(&radianceCacheContext, nullptr);

	radianceCacheContext = nullptr;
	radianceCachePredictionInputs = nullptr;
	radianceCachePredictionOutputs = nullptr;
	radianceCacheTrainInputs = nullptr;
	radianceCacheTrainTargets = nullptr;
	radianceCacheSampleCounters = nullptr;
	radianceCacheCounterUpload = nullptr;
	radianceCacheInputUpload = nullptr;
	radianceCacheTrainingInputUpload = nullptr;
	radianceCacheTrainingTargetUpload = nullptr;
	radianceCacheInputReadback = nullptr;
	radianceCacheOutputReadback = nullptr;

	radianceCacheBuffersReady = false;
	radianceCacheLastFenceValue = 0;
	radianceCacheCapturedInferencePending = false;
	radianceCacheCapturedInferenceFence = 0;
	radianceCacheCapturedInferenceSampleCount = 0;
	std::fill(
		std::begin(radianceCacheCapturedInferenceSourceIndices),
		std::end(radianceCacheCapturedInferenceSourceIndices),
		0u);
	radianceCacheResetPending.store(false);
}

bool FidelityFX::EnsureRadianceCacheContext()
{
	if (radianceCacheContext)
		return true;

	if (!featureRadianceCache ||
		radianceCacheFailureLatched ||
		!radianceCacheModule ||
		!ffxModule.CreateContext ||
		!ffxModule.DestroyContext)
		return false;

	// Interop/device unavailability can be transient during startup.
	// Do not latch that as a Radiance Cache provider failure.
	if (!EnsureRuntimeUpscalerInterop())
		return false;

	auto& sc = globals::features::upscaling.dx12SwapChain;
	if (!sc.d3d12Device)
		return false;

	ffxCreateContextDescRadianceCache create{};
	create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_RADIANCECACHE;
	create.flags = 0;
	create.version = FFX_RADIANCECACHE_VERSION;
	create.maxInferenceSampleCount = radianceCacheInferenceCapacity;
	create.maxTrainingSampleCount = radianceCacheTrainingCapacity;

	ffx::CreateBackendDX12Desc backend{};
	backend.device = sc.d3d12Device.get();
	create.header.pNext = &backend.header;

	const ffxReturnCode_t result =
		ffxModule.CreateContext(&radianceCacheContext, &create.header, nullptr);

	if (result != FFX_API_RETURN_OK || !radianceCacheContext) {
		logger::error(
			"[RadianceCache] FidelityFX Radiance Cache 0.9 context creation failed (return code {}); disabling Radiance Cache for this session",
			static_cast<int>(result));

		radianceCacheFailureLatched = true;
		ResetRadianceCache();
		return false;
	}

	return true;
}

void FidelityFX::ResetRayRegeneration()
{
	// Denoiser contexts own GPU resources referenced by the last dispatch.  AMD's
	// API requires those workloads to be complete before DestroyContext.  The RR
	// dispatch signals the shared runtime fence after its command list, so waiting
	// for the newest RR command-context value is sufficient and avoids destroying
	// provider state while it is still in flight.
	if (rayRegenerationContext && runtimeD3D12Fence) {
		uint64_t newestFence = 0;
		for (const auto& commandContext : runtimeCommandContexts)
			newestFence = std::max(newestFence, commandContext.fenceValue);

		if (newestFence != 0 && !WaitForRuntimeD3D12Fence(newestFence)) {
			logger::error(
				"[RayRegeneration] Timed out waiting for the last D3D12 dispatch; "
				"preserving the RR context and shared resources to avoid destroying "
				"provider state that may still be in flight");
			rayRegenerationFailureLatched = true;
			return;
		}
	}

	if (rayRegenerationContext && ffxModule.DestroyContext)
		(void)ffxModule.DestroyContext(&rayRegenerationContext, nullptr);
	rayRegenerationContext = nullptr;
	rayRegenerationWidth = rayRegenerationHeight = 0;
	DeleteWrapped(rrSharedSignal);
	DeleteWrapped(rrSharedDepth);
	DeleteWrapped(rrSharedMotion);
	DeleteWrapped(rrSharedNormal);
	DeleteWrapped(rrSharedSpecularAlbedo);
	DeleteWrapped(rrSharedDiffuseAlbedo);
	DeleteWrapped(rrSharedOutput);
}

bool FidelityFX::EnsureRayRegenerationResources(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	uint32_t width, uint32_t height)
{
	if (!featureRayRegeneration || rayRegenerationFailureLatched || !denoiserModule || !ffxModule.CreateContext || !ffxModule.Dispatch)
		return false;
	if (!signal || !depth || !motion || !normal || !specAlbedo || !diffAlbedo || !output || !width || !height)
		return false;
	if (!EnsureRuntimeUpscalerInterop())
		return false;

	if (rayRegenerationContext && rayRegenerationWidth == width && rayRegenerationHeight == height && rrSharedOutput)
		return true;

	ResetRayRegeneration();
	if (rayRegenerationContext) {
		// Reset can deliberately preserve an in-flight context after a fence timeout.
		// Do not replace its shared resources underneath it.
		return false;
	}
	auto& sc = globals::features::upscaling.dx12SwapChain;
	try {
		rrSharedSignal = new WrappedResource(SharedDesc(signal, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Signal");
		rrSharedDepth = new WrappedResource(SharedDesc(depth, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Depth");
		rrSharedMotion = new WrappedResource(SharedDesc(motion, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Motion");
		rrSharedNormal = new WrappedResource(SharedDesc(normal, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::NormalRoughness");
		rrSharedSpecularAlbedo = new WrappedResource(SharedDesc(specAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::SpecularAlbedo");
		rrSharedDiffuseAlbedo = new WrappedResource(SharedDesc(diffAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::DiffuseAlbedo");
		rrSharedOutput = new WrappedResource(SharedDesc(output, width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Output");

		ffxCreateContextDescDenoiser create{};
		create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
		create.version = FFX_DENOISER_VERSION;
		create.maxRenderSize = { width, height };
		create.signalFlags = FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
		create.checkerboardSignalFlags = 0;
		create.flags = 0;
		ffx::CreateBackendDX12Desc backend{};
		backend.device = sc.d3d12Device.get();
		create.header.pNext = &backend.header;
		if (ffxModule.CreateContext(&rayRegenerationContext, &create.header, nullptr) != FFX_API_RETURN_OK || !rayRegenerationContext) {
			logger::error("[RayRegeneration] FidelityFX Denoiser 1.2 context creation failed; disabling RR for this session");
			rayRegenerationFailureLatched = true;
			ResetRayRegeneration();
			return false;
		}
		rayRegenerationWidth = width;
		rayRegenerationHeight = height;
		logger::info("[RayRegeneration] FidelityFX Denoiser 1.2 context created at {}x{}", width, height);
		return true;
	} catch (const std::exception& e) {
		logger::error("[RayRegeneration] Resource/context setup failed: {}", e.what());
		ResetRayRegeneration();
		return false;
	}
}

bool FidelityFX::DispatchRayRegeneration(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	uint32_t width, uint32_t height, bool resetHistory)
{
	if (!EnsureRayRegenerationResources(signal, depth, motion, normal, specAlbedo, diffAlbedo, output, width, height))
		return false;
	auto& sc = globals::features::upscaling.dx12SwapChain;
	auto* cc = AcquireRuntimeCommandContext();
	if (!cc)
		return false;

	D3D11_BOX box{ 0, 0, 0, width, height, 1 };
	auto copy11 = [&](ID3D11Texture2D* src, WrappedResource* dst) { sc.d3d11Context->CopySubresourceRegion(dst->resource11, 0, 0, 0, 0, src, 0, &box); };
	bool commandListRecording = false;
	try {
		copy11(signal, rrSharedSignal);
		copy11(depth, rrSharedDepth);
		copy11(motion, rrSharedMotion);
		copy11(normal, rrSharedNormal);
		copy11(specAlbedo, rrSharedSpecularAlbedo);
		copy11(diffAlbedo, rrSharedDiffuseAlbedo);
		const uint64_t f11 = runtimeFenceValue++;
		DX::ThrowIfFailed(sc.d3d11Context->Signal(runtimeD3D11Fence.get(), f11));
		DX::ThrowIfFailed(sc.commandQueue->Wait(runtimeD3D12Fence.get(), f11));

		DX::ThrowIfFailed(cc->commandAllocator->Reset());
		DX::ThrowIfFailed(cc->commandList->Reset(cc->commandAllocator.get(), nullptr));
		commandListRecording = true;
		ID3D12Resource* reads[] = { rrSharedSignal->resource.get(), rrSharedDepth->resource.get(), rrSharedMotion->resource.get(), rrSharedNormal->resource.get(), rrSharedSpecularAlbedo->resource.get(), rrSharedDiffuseAlbedo->resource.get() };
		std::vector<D3D12_RESOURCE_BARRIER> begin;
		for (auto* r : reads) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(rrSharedOutput->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		cc->commandList->ResourceBarrier((UINT)begin.size(), begin.data());

		ffxDispatchDescDenoiser common{};
		common.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
		common.commandList = cc->commandList.get();
		common.linearDepth = ffxApiGetResourceDX12(rrSharedDepth->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.motionVectors = ffxApiGetResourceDX12(rrSharedMotion->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.normals = ffxApiGetResourceDX12(rrSharedNormal->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.specularAlbedo = ffxApiGetResourceDX12(rrSharedSpecularAlbedo->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.diffuseAlbedo = ffxApiGetResourceDX12(rrSharedDiffuseAlbedo->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.motionVectorScale = { 1.f, 1.f, 1.f };
		common.jitterOffsets = { 0.f, 0.f };
		const auto& curPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		const auto& prevPos = globals::game::frameBufferCached.GetCameraPreviousPosAdjust();
		common.cameraPositionDelta = { prevPos.x - curPos.x, prevPos.y - curPos.y, prevPos.z - curPos.z };
		common.view = ToFfxMatrix(globals::game::frameBufferCached.GetCameraView());
		common.projection = ToFfxMatrix(globals::game::frameBufferCached.GetCameraProjUnjittered());
		common.linearDepthBounds = { std::max(0.001f, *globals::game::cameraNear), *globals::game::cameraFar };
		common.renderSize = { width, height };
		common.frameIndex = (uint32_t)*globals::game::frameCounter;
		common.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO | (resetHistory ? FFX_DENOISER_DISPATCH_RESET : 0);

		ffxDispatchDescDenoiserIndirectSpecular spec{};
		spec.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
		spec.signal.input = ffxApiGetResourceDX12(rrSharedSignal->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		spec.signal.output = ffxApiGetResourceDX12(rrSharedOutput->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
		spec.signal.checkerboardOrigin = 0;
		common.header.pNext = &spec.header;
		// AMD provider code is outside our control. Match RuntimeUpscaler's SEH
		// boundary so an access violation in the provider is converted into a clean
		// RR fallback instead of taking Skyrim down with it.
		bool dispatchFaulted = false;
		const ffxReturnCode_t dispatchResult = DispatchDenoiserProtected(&rayRegenerationContext, &common.header, dispatchFaulted);
		if (dispatchFaulted || dispatchResult != FFX_API_RETURN_OK) {
			if (rrDiagnosticsEnabled)
				++rayRegenerationFailedDispatches;
			logger::error("[RayRegeneration] Denoiser dispatch {}; disabling RR for this session", dispatchFaulted ? "faulted inside the provider" : "returned failure");
			rayRegenerationFailureLatched = true;

			// Dispatch failed before submission. The recorded barriers never
			// reached the GPU, so close and discard this command list.
			if (commandListRecording) {
				const HRESULT closeResult = cc->commandList->Close();
				commandListRecording = false;
				if (FAILED(closeResult))
					logger::warn("[RayRegeneration] Failed to close abandoned D3D12 command list: 0x{:08X}", static_cast<unsigned>(closeResult));
			}
			return false;
		}

		std::vector<D3D12_RESOURCE_BARRIER> end;
		for (auto* r : reads) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(rrSharedOutput->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		cc->commandList->ResourceBarrier((UINT)end.size(), end.data());
		DX::ThrowIfFailed(cc->commandList->Close());
		commandListRecording = false;
		ID3D12CommandList* lists[] = { cc->commandList.get() };
		sc.commandQueue->ExecuteCommandLists(1, lists);
		const uint64_t f12 = runtimeFenceValue++;
		DX::ThrowIfFailed(sc.commandQueue->Signal(runtimeD3D12Fence.get(), f12));
		cc->fenceValue = f12;
		DX::ThrowIfFailed(sc.d3d11Context->Wait(runtimeD3D11Fence.get(), f12));
		sc.d3d11Context->CopySubresourceRegion(output, 0, 0, 0, 0, rrSharedOutput->resource11, 0, &box);

		if (rrDiagnosticsEnabled) {
			++rayRegenerationSuccessfulDispatches;

			if (resetHistory)
				++rayRegenerationHistoryResets;
		}

		return true;
	} catch (const std::exception& e) {
		if (commandListRecording) {
			// Reset succeeded, but recording failed before submission.
			// Close and discard the incomplete list.
			(void)cc->commandList->Close();
			commandListRecording = false;
		}
		if (rrDiagnosticsEnabled)
			++rayRegenerationFailedDispatches;
		logger::error("[RayRegeneration] Dispatch failed: {}", e.what());
		return false;
	}
}

void FidelityFX::ResetReflectionRayRegeneration()
{
	// Denoiser contexts own GPU resources referenced by the last dispatch.  AMD's
	// API requires those workloads to be complete before DestroyContext.  The RR
	// dispatch signals the shared runtime fence after its command list, so waiting
	// for the newest RR command-context value is sufficient and avoids destroying
	// provider state while it is still in flight.
	if (reflectionRayRegenerationContext && runtimeD3D12Fence) {
		uint64_t newestFence = 0;
		for (const auto& commandContext : runtimeCommandContexts)
			newestFence = std::max(newestFence, commandContext.fenceValue);

		if (newestFence != 0 && !WaitForRuntimeD3D12Fence(newestFence)) {
			logger::error(
				"[ReflectionRayRegeneration] Timed out waiting for the last D3D12 dispatch; "
				"preserving the RR context and shared resources to avoid destroying "
				"provider state that may still be in flight");
			rayRegenerationFailureLatched = true;
			return;
		}
	}

	if (reflectionRayRegenerationContext && ffxModule.DestroyContext)
		(void)ffxModule.DestroyContext(&reflectionRayRegenerationContext, nullptr);
	reflectionRayRegenerationContext = nullptr;
	reflectionRayRegenerationWidth = reflectionRayRegenerationHeight = 0;
	DeleteWrapped(reflectionRRSharedSignal);
	DeleteWrapped(reflectionRRSharedDepth);
	DeleteWrapped(reflectionRRSharedMotion);
	DeleteWrapped(reflectionRRSharedNormal);
	DeleteWrapped(reflectionRRSharedSpecularAlbedo);
	DeleteWrapped(reflectionRRSharedDiffuseAlbedo);
	DeleteWrapped(reflectionRRSharedOutput);
}

bool FidelityFX::EnsureReflectionRayRegenerationResources(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	uint32_t width, uint32_t height)
{
	if (!featureRayRegeneration || rayRegenerationFailureLatched || !denoiserModule || !ffxModule.CreateContext || !ffxModule.Dispatch)
		return false;
	if (!signal || !depth || !motion || !normal || !specAlbedo || !diffAlbedo || !output || !width || !height)
		return false;
	if (!EnsureRuntimeUpscalerInterop())
		return false;

	if (reflectionRayRegenerationContext && reflectionRayRegenerationWidth == width && reflectionRayRegenerationHeight == height && reflectionRRSharedOutput)
		return true;

	ResetReflectionRayRegeneration();
	if (reflectionRayRegenerationContext) {
		// Reset can deliberately preserve an in-flight context after a fence timeout.
		// Do not replace its shared resources underneath it.
		return false;
	}
	auto& sc = globals::features::upscaling.dx12SwapChain;
	try {
		reflectionRRSharedSignal = new WrappedResource(SharedDesc(signal, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "ReflectionRayRegeneration::Signal");
		reflectionRRSharedDepth = new WrappedResource(SharedDesc(depth, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "ReflectionRayRegeneration::Depth");
		reflectionRRSharedMotion = new WrappedResource(SharedDesc(motion, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "ReflectionRayRegeneration::Motion");
		reflectionRRSharedNormal = new WrappedResource(SharedDesc(normal, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "ReflectionRayRegeneration::NormalRoughness");
		reflectionRRSharedSpecularAlbedo = new WrappedResource(SharedDesc(specAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "ReflectionRayRegeneration::SpecularAlbedo");
		reflectionRRSharedDiffuseAlbedo = new WrappedResource(SharedDesc(diffAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "ReflectionRayRegeneration::DiffuseAlbedo");
		reflectionRRSharedOutput = new WrappedResource(SharedDesc(output, width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), sc.d3d11Device.get(), sc.d3d12Device.get(), "ReflectionRayRegeneration::Output");

		ffxCreateContextDescDenoiser create{};
		create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
		create.version = FFX_DENOISER_VERSION;
		create.maxRenderSize = { width, height };
		create.signalFlags = FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
		create.checkerboardSignalFlags = 0;
		create.flags = 0;
		ffx::CreateBackendDX12Desc backend{};
		backend.device = sc.d3d12Device.get();
		create.header.pNext = &backend.header;
		if (ffxModule.CreateContext(&reflectionRayRegenerationContext, &create.header, nullptr) != FFX_API_RETURN_OK || !reflectionRayRegenerationContext) {
			logger::error("[ReflectionRayRegeneration] FidelityFX Denoiser 1.2 context creation failed; disabling RR for this session");
			rayRegenerationFailureLatched = true;
			ResetReflectionRayRegeneration();
			return false;
		}
		reflectionRayRegenerationWidth = width;
		reflectionRayRegenerationHeight = height;
		logger::info("[ReflectionRayRegeneration] FidelityFX Denoiser 1.2 context created at {}x{}", width, height);
		return true;
	} catch (const std::exception& e) {
		logger::error("[ReflectionRayRegeneration] Resource/context setup failed: {}", e.what());
		ResetReflectionRayRegeneration();
		return false;
	}
}

bool FidelityFX::DispatchReflectionRayRegeneration(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	uint32_t width, uint32_t height, bool resetHistory)
{
	if (!EnsureReflectionRayRegenerationResources(signal, depth, motion, normal, specAlbedo, diffAlbedo, output, width, height))
		return false;
	auto& sc = globals::features::upscaling.dx12SwapChain;
	auto* cc = AcquireRuntimeCommandContext();
	if (!cc)
		return false;

	D3D11_BOX box{ 0, 0, 0, width, height, 1 };
	auto copy11 = [&](ID3D11Texture2D* src, WrappedResource* dst) { sc.d3d11Context->CopySubresourceRegion(dst->resource11, 0, 0, 0, 0, src, 0, &box); };
	bool commandListRecording = false;
	try {
		copy11(signal, reflectionRRSharedSignal);
		copy11(depth, reflectionRRSharedDepth);
		copy11(motion, reflectionRRSharedMotion);
		copy11(normal, reflectionRRSharedNormal);
		copy11(specAlbedo, reflectionRRSharedSpecularAlbedo);
		copy11(diffAlbedo, reflectionRRSharedDiffuseAlbedo);
		const uint64_t f11 = runtimeFenceValue++;
		DX::ThrowIfFailed(sc.d3d11Context->Signal(runtimeD3D11Fence.get(), f11));
		DX::ThrowIfFailed(sc.commandQueue->Wait(runtimeD3D12Fence.get(), f11));

		DX::ThrowIfFailed(cc->commandAllocator->Reset());
		DX::ThrowIfFailed(cc->commandList->Reset(cc->commandAllocator.get(), nullptr));
		commandListRecording = true;
		ID3D12Resource* reads[] = { reflectionRRSharedSignal->resource.get(), reflectionRRSharedDepth->resource.get(), reflectionRRSharedMotion->resource.get(), reflectionRRSharedNormal->resource.get(), reflectionRRSharedSpecularAlbedo->resource.get(), reflectionRRSharedDiffuseAlbedo->resource.get() };
		std::vector<D3D12_RESOURCE_BARRIER> begin;
		for (auto* r : reads) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(reflectionRRSharedOutput->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		cc->commandList->ResourceBarrier((UINT)begin.size(), begin.data());

		ffxDispatchDescDenoiser common{};
		common.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
		common.commandList = cc->commandList.get();
		common.linearDepth = ffxApiGetResourceDX12(reflectionRRSharedDepth->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.motionVectors = ffxApiGetResourceDX12(reflectionRRSharedMotion->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.normals = ffxApiGetResourceDX12(reflectionRRSharedNormal->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.specularAlbedo = ffxApiGetResourceDX12(reflectionRRSharedSpecularAlbedo->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.diffuseAlbedo = ffxApiGetResourceDX12(reflectionRRSharedDiffuseAlbedo->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.motionVectorScale = { 1.f, 1.f, 1.f };
		common.jitterOffsets = { 0.f, 0.f };
		const auto& curPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		const auto& prevPos = globals::game::frameBufferCached.GetCameraPreviousPosAdjust();
		common.cameraPositionDelta = { prevPos.x - curPos.x, prevPos.y - curPos.y, prevPos.z - curPos.z };
		common.view = ToFfxMatrix(globals::game::frameBufferCached.GetCameraView());
		common.projection = ToFfxMatrix(globals::game::frameBufferCached.GetCameraProjUnjittered());
		common.linearDepthBounds = { std::max(0.001f, *globals::game::cameraNear), *globals::game::cameraFar };
		common.renderSize = { width, height };
		common.frameIndex = (uint32_t)*globals::game::frameCounter;
		common.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO | (resetHistory ? FFX_DENOISER_DISPATCH_RESET : 0);

		ffxDispatchDescDenoiserIndirectSpecular spec{};
		spec.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
		spec.signal.input = ffxApiGetResourceDX12(reflectionRRSharedSignal->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		spec.signal.output = ffxApiGetResourceDX12(reflectionRRSharedOutput->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
		spec.signal.checkerboardOrigin = 0;
		common.header.pNext = &spec.header;
		// AMD provider code is outside our control. Match RuntimeUpscaler's SEH
		// boundary so an access violation in the provider is converted into a clean
		// RR fallback instead of taking Skyrim down with it.
		bool dispatchFaulted = false;
		const ffxReturnCode_t dispatchResult = DispatchDenoiserProtected(&reflectionRayRegenerationContext, &common.header, dispatchFaulted);
		if (dispatchFaulted || dispatchResult != FFX_API_RETURN_OK) {
			++reflectionRayRegenerationFailedDispatches;
			logger::error("[ReflectionRayRegeneration] Denoiser dispatch {}; disabling RR for this session", dispatchFaulted ? "faulted inside the provider" : "returned failure");
			rayRegenerationFailureLatched = true;

			// Dispatch failed before submission. The recorded barriers never
			// reached the GPU, so close and discard this command list.
			if (commandListRecording) {
				const HRESULT closeResult = cc->commandList->Close();
				commandListRecording = false;
				if (FAILED(closeResult))
					logger::warn("[ReflectionRayRegeneration] Failed to close abandoned D3D12 command list: 0x{:08X}", static_cast<unsigned>(closeResult));
			}
			return false;
		}

		std::vector<D3D12_RESOURCE_BARRIER> end;
		for (auto* r : reads) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(reflectionRRSharedOutput->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		cc->commandList->ResourceBarrier((UINT)end.size(), end.data());
		DX::ThrowIfFailed(cc->commandList->Close());
		commandListRecording = false;
		ID3D12CommandList* lists[] = { cc->commandList.get() };
		sc.commandQueue->ExecuteCommandLists(1, lists);
		const uint64_t f12 = runtimeFenceValue++;
		DX::ThrowIfFailed(sc.commandQueue->Signal(runtimeD3D12Fence.get(), f12));
		cc->fenceValue = f12;
		DX::ThrowIfFailed(sc.d3d11Context->Wait(runtimeD3D11Fence.get(), f12));
		sc.d3d11Context->CopySubresourceRegion(output, 0, 0, 0, 0, reflectionRRSharedOutput->resource11, 0, &box);

		++reflectionRayRegenerationSuccessfulDispatches;

		if (!reflectionRayRegenerationFirstSuccessLogged) {
			reflectionRayRegenerationFirstSuccessLogged = true;
			logger::info(
				"[ReflectionRayRegeneration] First FidelityFX Denoiser 1.2 dispatch succeeded at {}x{} "
				"(indirect specular, resetHistory={})",
				width,
				height,
				resetHistory);
		}

		return true;
	} catch (const std::exception& e) {
		if (commandListRecording) {
			// Reset succeeded, but recording failed before submission.
			// Close and discard the incomplete list.
			(void)cc->commandList->Close();
			commandListRecording = false;
		}
		++reflectionRayRegenerationFailedDispatches;
		logger::error("[ReflectionRayRegeneration] Dispatch failed: {}", e.what());
		return false;
	}
}
