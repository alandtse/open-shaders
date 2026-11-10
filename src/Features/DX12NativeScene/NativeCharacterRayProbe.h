#pragma once

#include "Features/DX12NativeScene/NativeCharacterIsolatedTLAS.h"

#include <directx/d3dx12.h>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
    // Report codes are shared with CharacterRayProbe.hlsl6:
    // 1 = no usable triangle; 2 = ray missed; 3 = committed triangle hit.
    struct CharacterRayOutcome
    {
        uint32_t status = 0, primitiveIndex = UINT32_MAX, instanceID = UINT32_MAX;
        float hitDistance = 0.0f;
    };

    struct CharacterRayResult
    {
        std::shared_ptr<const CharacterIsolatedTLAS> scene;
        winrt::com_ptr<ID3D12Resource> output, readback;
        winrt::com_ptr<ID3D12Fence> completion;
        uint64_t fenceValue = 0, allocationBytes = 0;
        uint32_t poseFrame = 0, partCount = 0;

        bool Ready() const
        {
            if (!completion || !fenceValue) return false;
            const uint64_t done = completion->GetCompletedValue();
            return done != UINT64_MAX && done >= fenceValue;
        }
    };

    struct CharacterRayPipeline
    {
        winrt::com_ptr<ID3D12Device> device;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> state;

        explicit CharacterRayPipeline(ID3D12Device* gpu)
        {
            if (!gpu) throw std::runtime_error("Character RayQuery device missing");
            device.copy_from(gpu);
            std::ifstream shader("Data/Shaders/DX12NativeScene/CharacterRayProbe.dxil",
                std::ios::binary | std::ios::ate);
            if (!shader)
                throw std::runtime_error("Isolated character RayQuery DXIL not installed");
            const auto length = shader.tellg();
            if (length <= 0 || length > 4 * 1024 * 1024)
                throw std::runtime_error("Isolated RayQuery DXIL size invalid");
            std::vector<char> code(static_cast<size_t>(length));
            shader.seekg(0);
            if (!shader.read(code.data(), std::streamsize(code.size())))
                throw std::runtime_error("Unable to read isolated RayQuery DXIL");

            D3D12_ROOT_PARAMETER params[5]{};
            for (uint32_t i = 0; i < 3; ++i) {
                params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                params[i].Descriptor.ShaderRegister = i;
            }
            params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[3].Descriptor.ShaderRegister = 0;
            params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[4].Constants = { 0, 0, 5 };
            const D3D12_ROOT_SIGNATURE_DESC desc{
                5, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            winrt::com_ptr<ID3DBlob> serialized, errors;
            if (FAILED(D3D12SerializeRootSignature(&desc,
                D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put())))
                throw std::runtime_error("Character RayQuery root signature invalid");
            if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                serialized->GetBufferSize(), IID_PPV_ARGS(root.put()))))
                throw std::runtime_error("Character RayQuery root creation failed");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = root.get();
            pso.CS = { code.data(), code.size() };
            if (FAILED(device->CreateComputePipelineState(&pso,
                IID_PPV_ARGS(state.put()))))
                throw std::runtime_error("Character RayQuery pipeline creation failed");
        }
    };

    struct CharacterRaySubmission
    {
        std::shared_ptr<const CharacterRayResult> result;
        std::shared_ptr<CharacterRayPipeline> pipeline;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList> commands;
    };

    struct CharacterRayRetirement
    {
        inline static std::mutex mutex;
        inline static std::vector<std::shared_ptr<CharacterRaySubmission>> inflight;

        static void ReapLocked()
        {
            std::erase_if(inflight, [](const auto& submitted) {
                return !submitted || !submitted->result ||
                    submitted->result->Ready();
            });
        }

        static bool CanSubmit()
        {
            std::scoped_lock lock(mutex);
            ReapLocked();
            return inflight.size() < 3;
        }

        static uint32_t Pending()
        {
            std::scoped_lock lock(mutex);
            ReapLocked();
            return uint32_t(inflight.size());
        }

        static void Retain(std::shared_ptr<CharacterRaySubmission> submission)
        {
            std::scoped_lock lock(mutex);
            inflight.emplace_back(std::move(submission));
        }
    };

    /** Read every independently targeted instance, after GPU completion. */
    inline std::optional<std::vector<CharacterRayOutcome>> ReadCharacterRayOutcomes(
        const std::shared_ptr<const CharacterRayResult>& result)
    {
        if (!result || !result->Ready() || !result->readback ||
            !result->partCount || result->partCount > 8u)
            return std::nullopt;
        const size_t bytes = size_t(result->partCount) * 16u;
        const D3D12_RANGE readRange{ 0, bytes };
        void* data = nullptr;
        if (FAILED(result->readback->Map(0, &readRange, &data)) || !data)
            return std::nullopt;
        std::vector<CharacterRayOutcome> all;
        all.reserve(result->partCount);
        for (uint32_t i = 0; i < result->partCount; ++i) {
            uint32_t values[4]{};
            std::memcpy(values, static_cast<const uint8_t*>(data) +
                size_t(i) * sizeof(values), sizeof(values));
            CharacterRayOutcome resultPart;
            resultPart.status = values[0];
            resultPart.hitDistance = std::bit_cast<float>(values[1]);
            resultPart.primitiveIndex = values[2];
            resultPart.instanceID = values[3];
            // A hit on another part is NOT proof for this instance.
            if (resultPart.status < 1u || resultPart.status > 3u ||
                (resultPart.status == 3u &&
                 (!std::isfinite(resultPart.hitDistance) ||
                  resultPart.hitDistance < 0.0f || resultPart.instanceID != i)))
                resultPart.status = 0u;
            all.emplace_back(resultPart);
        }
        result->readback->Unmap(0, nullptr);
        return all;
    }

    inline std::optional<CharacterRayOutcome> ReadCharacterRayOutcome(
        const std::shared_ptr<const CharacterRayResult>& result)
    {
        const auto outcomes = ReadCharacterRayOutcomes(result);
        if (!outcomes || outcomes->empty()) return std::nullopt;
        return outcomes->front();
    }

    /** A one-ray diagnostic only. The isolated TLAS and all buffers are
     * retained independently. The world TLAS and shadow resources are not
     * bound to or modified by the command list.
     */
    inline std::shared_ptr<const CharacterRayResult> DispatchCharacterRayProbe(
        ID3D12Device* device, ID3D12CommandQueue* queue,
        std::shared_ptr<const CharacterIsolatedTLAS> scene)
    {
        if (!device || !queue || !scene || !scene->Ready() ||
            !scene->acceleration || scene->bottoms.empty() ||
            scene->bottoms.size() > 8u || !CharacterRayRetirement::CanSubmit())
            return {};

        struct RayInput
        {
            std::shared_ptr<const CharacterDeformedResult> pose;
            std::shared_ptr<const CharacterStagedMesh> mesh;
        };
        std::vector<RayInput> rayInputs;
        rayInputs.reserve(scene->bottoms.size());
        for (const auto& bottom : scene->bottoms) {
            if (!bottom || !bottom->Ready() || !bottom->deformation ||
                !bottom->deformation->Ready() ||
                !bottom->deformation->input || !bottom->deformation->positions)
                return {};
            const auto deformation = bottom->deformation;
            std::shared_ptr<const CharacterStagedMesh> mesh;
            for (const auto& entry : deformation->input->meshes) {
                if (entry->skinned &&
                    entry->actorFormID == bottom->actorFormID &&
                    entry->partOrdinal == bottom->partOrdinal &&
                    entry->sourceToken == bottom->sourceToken &&
                    entry->source && entry->vertices >= 3 &&
                    entry->indices >= 3 && entry->indices % 3u == 0u &&
                    entry->indexOffset <= UINT32_MAX) {
                    mesh = entry;
                    break;
                }
            }
            if (!mesh) return {};
            const uint64_t sourceBytes = mesh->source->GetDesc().Width;
            const uint64_t indexBytes = uint64_t(mesh->indices) * 4ull;
            if (mesh->indexOffset > sourceBytes ||
                indexBytes > sourceBytes - mesh->indexOffset)
                return {};
            rayInputs.push_back({ deformation, mesh });
        }

        static std::mutex pipelineMutex;
        static std::shared_ptr<CharacterRayPipeline> cachedPipeline;
        std::shared_ptr<CharacterRayPipeline> pipeline;
        {
            std::scoped_lock lock(pipelineMutex);
            if (!cachedPipeline || cachedPipeline->device.get() != device)
                cachedPipeline = std::make_shared<CharacterRayPipeline>(device);
            pipeline = cachedPipeline;
        }

        const uint64_t kResultSize = 16ull * rayInputs.size();
        const auto defaultDesc = CD3DX12_RESOURCE_DESC::Buffer(
            kResultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto readDesc = CD3DX12_RESOURCE_DESC::Buffer(kResultSize);
        const uint64_t outputBytes = device->GetResourceAllocationInfo(
            0, 1, &defaultDesc).SizeInBytes;
        const uint64_t readBytes = device->GetResourceAllocationInfo(
            0, 1, &readDesc).SizeInBytes;
        if (!outputBytes || outputBytes == UINT64_MAX ||
            !readBytes || readBytes == UINT64_MAX ||
            outputBytes > UINT64_MAX - readBytes)
            return {};

        uint64_t known = scene->allocationBytes;
        for (const auto& bottom : scene->bottoms) {
            const auto deformation = bottom->deformation;
            for (const uint64_t bytes : {
                bottom->allocationBytes, deformation->allocationBytes,
                deformation->input->residentBytes }) {
                if (bytes > UINT64_MAX - known) return {};
                known += bytes;
            }
        }
        VideoMemoryBudget memory;
        const auto budget = memory.Query(device, known, 0);
        if (!budget.dxgiAvailable || known > budget.effectiveBytes ||
            outputBytes + readBytes > budget.effectiveBytes - known)
            return {};

        auto probe = std::make_shared<CharacterRayResult>();
        probe->scene = std::move(scene);
        probe->poseFrame = probe->scene->poseFrame;
        probe->partCount = uint32_t(rayInputs.size());
        probe->allocationBytes = outputBytes + readBytes;
        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto readHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &defaultDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(probe->output.put()))) ||
            FAILED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE,
                &readDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(probe->readback.put()))))
            throw std::runtime_error("Character RayQuery diagnostic buffer allocation failed");

        auto submitted = std::make_shared<CharacterRaySubmission>();
        submitted->result = probe;
        submitted->pipeline = std::move(pipeline);
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(submitted->allocator.put()))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                submitted->allocator.get(), submitted->pipeline->state.get(),
                IID_PPV_ARGS(submitted->commands.put()))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(probe->completion.put()))))
            throw std::runtime_error("Character RayQuery diagnostic commands unavailable");

        auto* commands = submitted->commands.get();
        commands->SetComputeRootSignature(submitted->pipeline->root.get());
        commands->SetComputeRootShaderResourceView(0,
            probe->scene->acceleration->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(3,
            probe->output->GetGPUVirtualAddress());
        for (size_t i = 0; i < rayInputs.size(); ++i) {
            const auto& input = rayInputs[i];
            // Isolated skinning/BLAS leave the position buffer in UAV state.
            const auto positionRead = CD3DX12_RESOURCE_BARRIER::Transition(
                input.pose->positions.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &positionRead);
            commands->SetComputeRootShaderResourceView(1,
                input.pose->positions->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(2,
                input.mesh->source->GetGPUVirtualAddress());
            const uint32_t constants[5]{
                input.mesh->vertices, input.mesh->indices,
                uint32_t(input.mesh->indexOffset),
                1u << uint32_t(i), uint32_t(i * 16u) };
            commands->SetComputeRoot32BitConstants(4, 5, constants, 0);
            commands->Dispatch(1, 1, 1);
            const auto restore = CD3DX12_RESOURCE_BARRIER::Transition(
                input.pose->positions.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->ResourceBarrier(1, &restore);
        }
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(probe->output.get());
        commands->ResourceBarrier(1, &barrier);
        const auto copyReady = CD3DX12_RESOURCE_BARRIER::Transition(
            probe->output.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->ResourceBarrier(1, &copyReady);
        commands->CopyBufferRegion(probe->readback.get(), 0,
            probe->output.get(), 0, kResultSize);
        if (FAILED(commands->Close()))
            throw std::runtime_error("Character RayQuery diagnostic list invalid");
        ID3D12CommandList* lists[]{ commands };
        queue->ExecuteCommandLists(1, lists);
        probe->fenceValue = 1;
        CharacterRayRetirement::Retain(submitted);
        if (FAILED(queue->Signal(probe->completion.get(), probe->fenceValue)))
            throw std::runtime_error("Character RayQuery diagnostic fence signal failed");
        return probe;
    }
}
