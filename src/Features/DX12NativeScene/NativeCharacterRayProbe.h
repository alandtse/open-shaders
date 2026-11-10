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
        uint32_t poseFrame = 0;

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
            params[4].Constants = { 0, 0, 4 };
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

    /** GPU-fence-complete readback, only from native CPU worker; never waits. */
    inline std::optional<CharacterRayOutcome> ReadCharacterRayOutcome(
        const std::shared_ptr<const CharacterRayResult>& result)
    {
        if (!result || !result->Ready() || !result->readback)
            return std::nullopt;
        const D3D12_RANGE readRange{ 0, 16 };
        void* data = nullptr;
        if (FAILED(result->readback->Map(0, &readRange, &data)) || !data)
            return std::nullopt;
        uint32_t output[4]{};
        std::memcpy(output, data, sizeof(output));
        result->readback->Unmap(0, nullptr);

        CharacterRayOutcome outcome;
        outcome.status = output[0];
        outcome.hitDistance = std::bit_cast<float>(output[1]);
        outcome.primitiveIndex = output[2];
        outcome.instanceID = output[3];
        if (outcome.status < 1 || outcome.status > 3)
            outcome.status = 0;
        if (outcome.status == 3 &&
            (!std::isfinite(outcome.hitDistance) || outcome.hitDistance < 0))
            outcome.status = 0;
        return outcome;
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
            !scene->bottom || !scene->bottom->deformation ||
            !scene->acceleration || !CharacterRayRetirement::CanSubmit())
            return {};

        const auto deformation = scene->bottom->deformation;
        std::shared_ptr<const CharacterStagedMesh> mesh;
        for (const auto& entry : deformation->input->meshes) {
            if (entry->skinned && entry->actorFormID == scene->actorFormID &&
                entry->partOrdinal == scene->partOrdinal &&
                entry->sourceToken == scene->bottom->sourceToken &&
                entry->source && entry->vertices >= 3 && entry->indices >= 3 &&
                entry->indexOffset <= UINT32_MAX &&
                entry->indices % 3u == 0u) {
                mesh = entry;
                break;
            }
        }
        if (!mesh) return {};
        const uint64_t inputBytes = mesh->source->GetDesc().Width;
        const uint64_t requiredIndexBytes = uint64_t(mesh->indices) * 4ull;
        if (mesh->indexOffset > inputBytes ||
            requiredIndexBytes > inputBytes - mesh->indexOffset)
            return {};

        static std::mutex pipelineMutex;
        static std::shared_ptr<CharacterRayPipeline> cachedPipeline;
        std::shared_ptr<CharacterRayPipeline> pipeline;
        {
            std::scoped_lock lock(pipelineMutex);
            if (!cachedPipeline || cachedPipeline->device.get() != device)
                cachedPipeline = std::make_shared<CharacterRayPipeline>(device);
            pipeline = cachedPipeline;
        }

        constexpr uint64_t kResultSize = 16;
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
        if (scene->bottom->allocationBytes > UINT64_MAX - known)
            return {};
        known += scene->bottom->allocationBytes;
        if (deformation->allocationBytes > UINT64_MAX - known)
            return {};
        known += deformation->allocationBytes;
        if (deformation->input &&
            deformation->input->residentBytes <= UINT64_MAX - known)
            known += deformation->input->residentBytes;
        else
            return {};
        VideoMemoryBudget memory;
        const auto budget = memory.Query(device, known, 0);
        if (!budget.dxgiAvailable || known > budget.effectiveBytes ||
            outputBytes + readBytes > budget.effectiveBytes - known)
            return {};

        auto probe = std::make_shared<CharacterRayResult>();
        probe->scene = std::move(scene);
        probe->poseFrame = probe->scene->poseFrame;
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
        // Isolated skinning/BLAS leave the position buffer in UAV state.
        // A raw ByteAddressBuffer read requires non-pixel SRV state.
        const auto positionRead = CD3DX12_RESOURCE_BARRIER::Transition(
            deformation->positions.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &positionRead);
        commands->SetComputeRootSignature(submitted->pipeline->root.get());
        commands->SetComputeRootShaderResourceView(0,
            probe->scene->acceleration->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(1,
            deformation->positions->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(2,
            mesh->source->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(3,
            probe->output->GetGPUVirtualAddress());
        const uint32_t constants[4]{
            mesh->vertices, mesh->indices, uint32_t(mesh->indexOffset), 0u };
        commands->SetComputeRoot32BitConstants(4, 4, constants, 0);
        commands->Dispatch(1, 1, 1);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(probe->output.get());
        commands->ResourceBarrier(1, &barrier);
        const auto copyReady = CD3DX12_RESOURCE_BARRIER::Transition(
            probe->output.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->ResourceBarrier(1, &copyReady);
        commands->CopyBufferRegion(probe->readback.get(), 0,
            probe->output.get(), 0, kResultSize);
        const auto positionRestore = CD3DX12_RESOURCE_BARRIER::Transition(
            deformation->positions.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->ResourceBarrier(1, &positionRestore);
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
