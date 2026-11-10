#pragma once

#include "Features/DX12NativeScene/NativeCharacterGeometryPreparation.h"

#include <directx/d3dx12.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
    struct CharacterDeformedResult
    {
        std::shared_ptr<const CharacterMeshGeneration> input;
        winrt::com_ptr<ID3D12Resource> positions;
        // Optional bounded debug readback; never used as a shading input.
        winrt::com_ptr<ID3D12Resource> validationReadback;
        uint64_t validationBytes = 0;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList> commands;
        winrt::com_ptr<ID3D12Fence> completion;
        uint64_t readyValue = 0, allocationBytes = 0;
        uint32_t actorFormID = 0, partOrdinal = 0, poseFrame = 0, vertices = 0;
        bool Ready() const
        {
            if (!completion || !readyValue) return false;
            const uint64_t done = completion->GetCompletedValue();
            return done != UINT64_MAX && done >= readyValue;
        }
    };

    // Sanity checks only: this does not prove parity with Skyrim's raster
    // skinning or establish native character-ray eligibility.
    struct CharacterDeformValidation
    {
        uint32_t poseFrame = 0, inspected = 0, nonFinite = 0;
        std::array<float, 3> minimum{}, maximum{};
        float maximumBindDisplacement = 0.0f;
        bool bindReferenceAvailable = false;
        bool sane = false;
    };

    // Called exclusively on the independent CPU worker, AFTER the GPU fence
    // completes. Readback is optional and at most 786,420 bytes per mesh.
    inline std::optional<CharacterDeformValidation> InspectCharacterDeformation(
        const std::shared_ptr<const CharacterDeformedResult>& frame)
    {
        if (!frame || !frame->Ready() || !frame->validationReadback ||
            !frame->vertices || frame->validationBytes != uint64_t(frame->vertices) * 12ull)
            return std::nullopt;
        const D3D12_RANGE bytesRead{ 0, SIZE_T(frame->validationBytes) };
        void* gpuBytes = nullptr;
        if (FAILED(frame->validationReadback->Map(0, &bytesRead, &gpuBytes)) ||
            !gpuBytes) return std::nullopt;

        std::shared_ptr<const CharacterStagedMesh> source;
        for (const auto& candidate : frame->input->meshes)
            if (candidate->skinned && candidate->actorFormID == frame->actorFormID &&
                candidate->partOrdinal == frame->partOrdinal &&
                candidate->sourceToken == frame->input->pose->sourceToken)
                { source = candidate; break; }
        void* bindBytes = nullptr;
        if (source && source->source) {
            const D3D12_RANGE bindRead{ 0,
                SIZE_T(source->vertexStride) * SIZE_T(frame->vertices) };
            if (FAILED(source->source->Map(0, &bindRead, &bindBytes)))
                bindBytes = nullptr;
        }

        CharacterDeformValidation result;
        result.poseFrame = frame->poseFrame;
        result.bindReferenceAvailable = bindBytes != nullptr;
        result.minimum.fill(std::numeric_limits<float>::infinity());
        result.maximum.fill(-std::numeric_limits<float>::infinity());
        constexpr uint32_t maximumSamples = 1024;
        const uint32_t count = std::min(frame->vertices, maximumSamples);
        for (uint32_t i = 0; i < count; ++i) {
            // Deterministic sampling covers the full vertex stream.
            const uint32_t vertex = uint32_t(uint64_t(i) * frame->vertices / count);
            float position[3]{};
            std::memcpy(position,
                static_cast<const uint8_t*>(gpuBytes) + size_t(vertex) * 12,
                sizeof(position));
            ++result.inspected;
            if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
                !std::isfinite(position[2])) {
                ++result.nonFinite;
                continue;
            }
            for (uint32_t axis = 0; axis < 3; ++axis) {
                result.minimum[axis] = std::min(result.minimum[axis], position[axis]);
                result.maximum[axis] = std::max(result.maximum[axis], position[axis]);
            }
            if (bindBytes) {
                float bind[3]{};
                std::memcpy(bind,
                    static_cast<const uint8_t*>(bindBytes) +
                        size_t(vertex) * source->vertexStride, sizeof(bind));
                const float dx = position[0] - bind[0];
                const float dy = position[1] - bind[1];
                const float dz = position[2] - bind[2];
                const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (std::isfinite(distance))
                    result.maximumBindDisplacement =
                        std::max(result.maximumBindDisplacement, distance);
                else
                    ++result.nonFinite;
            }
        }
        if (bindBytes) {
            const D3D12_RANGE noWrite{ 0, 0 };
            source->source->Unmap(0, &noWrite);
        }
        const D3D12_RANGE noWrite{ 0, 0 };
        frame->validationReadback->Unmap(0, &noWrite);
        result.sane = result.inspected > 0 && result.nonFinite == 0;
        return result;
    }

    // A reset may drop published scene references while an isolated compute
    // dispatch is in flight. Retain the input, output, allocator and fence
    // together until the GPU really finishes. No render-thread CPU waits.
    struct CharacterDeformationRetirement
    {
        inline static std::mutex mutex;
        inline static std::vector<std::shared_ptr<const CharacterDeformedResult>> inFlight;

        static bool CanSubmit()
        {
            std::scoped_lock lock(mutex);
            std::erase_if(inFlight, [](const auto& frame) {
                return !frame || frame->Ready() ||
                    (frame->completion && frame->completion->GetCompletedValue() == UINT64_MAX);
            });
            return inFlight.size() < 8;
        }

        static void Retain(std::shared_ptr<const CharacterDeformedResult> frame)
        {
            std::scoped_lock lock(mutex);
            inFlight.push_back(std::move(frame));
        }

        static size_t InFlight()
        {
            std::scoped_lock lock(mutex);
            return std::count_if(inFlight.begin(), inFlight.end(),
                [](const auto& frame) { return frame && !frame->Ready(); });
        }
    };

    struct CharacterSkinningPipeline
    {
        winrt::com_ptr<ID3D12Device> device;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> state;

        explicit CharacterSkinningPipeline(ID3D12Device* gpu)
        {
            if (!gpu) throw std::runtime_error("Character DX12 device unavailable");
            device.copy_from(gpu);
            std::ifstream shader("Data/Shaders/DX12NativeScene/CharacterSkinning.dxil",
                std::ios::binary | std::ios::ate);
            if (!shader) throw std::runtime_error("Native character DXIL is not installed");
            const std::streamoff size = shader.tellg();
            if (size <= 0 || size > 4 * 1024 * 1024)
                throw std::runtime_error("Native character skinning DXIL size invalid");
            std::vector<char> code(static_cast<size_t>(size));
            shader.seekg(0);
            if (!shader.read(code.data(), std::streamsize(code.size())))
                throw std::runtime_error("Native character DXIL could not be read");

            D3D12_ROOT_PARAMETER params[4]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[0].Descriptor.ShaderRegister = 0;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[1].Descriptor.ShaderRegister = 1;
            params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[2].Descriptor.ShaderRegister = 0;
            params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[3].Constants = { 0, 0, 4 };
            const D3D12_ROOT_SIGNATURE_DESC desc{ 4, params, 0, nullptr,
                D3D12_ROOT_SIGNATURE_FLAG_NONE };
            winrt::com_ptr<ID3DBlob> serialized, errors;
            if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                serialized.put(), errors.put())))
                throw std::runtime_error("Native character root signature serialization failed");
            if (FAILED(gpu->CreateRootSignature(0,
                serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(root.put()))))
                throw std::runtime_error("Native character root signature creation failed");
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
            ps.pRootSignature = root.get();
            ps.CS = { code.data(), code.size() };
            if (FAILED(gpu->CreateComputePipelineState(&ps, IID_PPV_ARGS(state.put()))))
                throw std::runtime_error("Native character skinning pipeline creation failed");
        }
    };

    /** @brief Dispatch a non-production DX12 skinning experiment.
     * Both inputs are immutable upload-heap SRVs; the output is a private
     * default-heap UAV. Only fence-complete results may be examined. This
     * function never changes the world's TLAS or raster character path.
     */
    inline std::shared_ptr<const CharacterDeformedResult> DispatchCharacterSkinning(
        ID3D12Device* device, ID3D12CommandQueue* queue,
        std::shared_ptr<const CharacterMeshGeneration> input,
        bool validateOutput = false)
    {
        if (!device || !queue || !input || !input->pose ||
            !CharacterDeformationRetirement::CanSubmit()) return {};
        std::shared_ptr<const CharacterStagedMesh> mesh;
        for (const auto& candidate : input->meshes)
            if (candidate->skinned && candidate->actorFormID == input->pose->actorFormID &&
                candidate->partOrdinal == input->pose->partOrdinal &&
                candidate->sourceToken == input->pose->sourceToken &&
                candidate->source && candidate->indices >= 3 && candidate->vertices)
                { mesh = candidate; break; }
        if (!mesh || !input->pose->registers) return {};

        // One native worker submits all character experiments. A cached
        // pipeline is recreated if the DX12 device changes.
        static std::mutex pipelineMutex;
        static std::shared_ptr<CharacterSkinningPipeline> pipeline;
        std::shared_ptr<CharacterSkinningPipeline> localPipeline;
        {
            std::scoped_lock lock(pipelineMutex);
            if (!pipeline || pipeline->device.get() != device)
                pipeline = std::make_shared<CharacterSkinningPipeline>(device);
            localPipeline = pipeline;
        }

        VideoMemoryBudget budget;
        const auto limit = budget.Query(device, input->residentBytes, 0);
        const auto maxBytes = std::min(limit.effectiveBytes, 16ull * 1024 * 1024);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(
            uint64_t(mesh->vertices) * 12ull + 4ull,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
        if (!limit.dxgiAvailable || info.SizeInBytes == UINT64_MAX ||
            info.SizeInBytes > maxBytes || input->residentBytes > maxBytes - info.SizeInBytes)
            return {};

        auto result = std::make_shared<CharacterDeformedResult>();
        result->input = std::move(input);
        result->actorFormID = mesh->actorFormID;
        result->partOrdinal = mesh->partOrdinal;
        result->poseFrame = result->input->pose->captureFrame;
        result->vertices = mesh->vertices;
        result->allocationBytes = info.SizeInBytes;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(result->positions.put()))))
            throw std::runtime_error("Native character deformed buffer allocation failed");
        result->positions->SetName(L"DX12NativeCharacter::DeformedPositions");
        if (validateOutput) {
            // Separate system-memory readback allocation. The GPU copy occurs
            // on the SAME queue and before the existing completion fence.
            const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            const auto readbackDesc =
                CD3DX12_RESOURCE_DESC::Buffer(uint64_t(mesh->vertices) * 12ull);
            if (FAILED(device->CreateCommittedResource(&readbackHeap,
                D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(result->validationReadback.put()))))
                throw std::runtime_error("Native character validation readback unavailable");
            result->validationBytes = uint64_t(mesh->vertices) * 12ull;
            result->validationReadback->SetName(L"DX12NativeCharacter::ValidationReadback");
        }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(result->allocator.put()))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                result->allocator.get(), localPipeline->state.get(),
                IID_PPV_ARGS(result->commands.put()))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(result->completion.put()))))
            throw std::runtime_error("Native character dispatch resources unavailable");

        auto* commands = result->commands.get();
        commands->SetComputeRootSignature(localPipeline->root.get());
        commands->SetComputeRootShaderResourceView(0, mesh->source->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(1,
            result->input->pose->registers->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(2,
            result->positions->GetGPUVirtualAddress());
        const uint32_t constants[4]{
            mesh->vertices, result->input->pose->boneCount,
            result->input->pose->registerCount, 0u };
        commands->SetComputeRoot32BitConstants(3, 4, constants, 0);
        commands->Dispatch((mesh->vertices + 63u) / 64u, 1, 1);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(result->positions.get());
        commands->ResourceBarrier(1, &barrier);
        if (result->validationReadback) {
            const auto copySource = CD3DX12_RESOURCE_BARRIER::Transition(
                result->positions.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1, &copySource);
            commands->CopyBufferRegion(result->validationReadback.get(), 0,
                result->positions.get(), 0, result->validationBytes);
            const auto restore = CD3DX12_RESOURCE_BARRIER::Transition(
                result->positions.get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->ResourceBarrier(1, &restore);
        }
        if (FAILED(commands->Close()))
            throw std::runtime_error("Native character skinning command list invalid");
        ID3D12CommandList* list[]{ commands };
        queue->ExecuteCommandLists(1, list);
        result->readyValue = 1;
        // Register before signalling; after submission, resources may never
        // be released by the worker until the fence is completed.
        CharacterDeformationRetirement::Retain(result);
        if (FAILED(queue->Signal(result->completion.get(), result->readyValue)))
            throw std::runtime_error("Native character skinning fence signal failed");
        return result;
    }
}
