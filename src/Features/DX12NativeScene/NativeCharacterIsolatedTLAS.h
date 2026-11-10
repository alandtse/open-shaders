#pragma once

#include "Features/DX12NativeScene/NativeCharacterIsolatedBLAS.h"

#include <directx/d3dx12.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
    // Separate from the published world TLAS and ShadowBindings. Every
    // diagnostic skinned instance has an identity transform until world-space
    // parity is proven; none are admitted to the production scene in
    // multi-part mode.
    struct CharacterIsolatedTLAS
    {
        std::shared_ptr<const CharacterIsolatedBLAS> bottom;
        std::vector<std::shared_ptr<const CharacterIsolatedBLAS>> bottoms;
        std::vector<std::array<float, 12>> instanceTransforms;
        winrt::com_ptr<ID3D12Resource> acceleration;
        winrt::com_ptr<ID3D12Fence> completion;
        uint64_t fenceValue = 0, allocationBytes = 0, scratchBytes = 0;
        uint32_t poseFrame = 0, actorFormID = 0, partOrdinal = 0;

        bool Ready() const
        {
            if (!completion || !fenceValue) return false;
            const uint64_t completed = completion->GetCompletedValue();
            return completed != UINT64_MAX && completed >= fenceValue;
        }
    };

    struct CharacterTLASSubmission
    {
        std::shared_ptr<const CharacterIsolatedTLAS> result;
        winrt::com_ptr<ID3D12Resource> instanceUpload, scratch;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
    };

    // Keep all resources of an in-flight TLAS build alive across scene reset,
    // UI toggles and worker publication. Never wait on a fence on the
    // Skyrim render thread.
    struct CharacterTLASRetirement
    {
        inline static std::mutex mutex;
        inline static std::vector<std::shared_ptr<CharacterTLASSubmission>> inflight;

        static void Reap()
        {
            std::erase_if(inflight, [](const auto& submission) {
                return !submission || !submission->result ||
                    submission->result->Ready();
            });
        }

        static bool CanSubmit()
        {
            std::scoped_lock lock(mutex);
            Reap();
            return inflight.size() < 3;
        }

        static uint32_t Pending()
        {
            std::scoped_lock lock(mutex);
            Reap();
            return uint32_t(inflight.size());
        }

        static void Retain(std::shared_ptr<CharacterTLASSubmission> submission)
        {
            std::scoped_lock lock(mutex);
            inflight.emplace_back(std::move(submission));
        }
    };

    /** @brief Isolated, bounded multi-instance character TLAS.
     * Production caster eligibility refuses multi-part mode until all
     * per-part pose and material contracts are validated.
     */
    inline std::shared_ptr<const CharacterIsolatedTLAS> BuildCharacterMultiPartTLAS(
        ID3D12Device* device, ID3D12CommandQueue* queue,
        std::vector<std::shared_ptr<const CharacterIsolatedBLAS>> bottoms,
        std::vector<std::array<float, 12>> instanceTransforms = {},
        uint32_t observationFrame = 0)
    {
        if (!device || !queue || bottoms.empty() || bottoms.size() > 8 ||
            !CharacterTLASRetirement::CanSubmit()) return {};
        const std::array<float, 12> identity{
            1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f };
        if (instanceTransforms.empty())
            instanceTransforms.assign(bottoms.size(), identity);
        if (instanceTransforms.size() != bottoms.size()) return {};
        for (size_t i = 0; i < bottoms.size(); ++i) {
            const auto& part = bottoms[i];
            if (!part || !part->Ready() || !part->acceleration)
                return {};
            if (part->rigidSource) {
                if (!part->rigidSource->source || part->rigidSource->skinned ||
                    part->rigidSource->sourceToken != part->sourceToken)
                    return {};
            } else if (!part->deformation || !part->validation ||
                !part->validation->sane ||
                part->validation->poseFrame != part->poseFrame) {
                return {};
            }
            for (const float component : instanceTransforms[i])
                if (!std::isfinite(component)) return {};
        }

        winrt::com_ptr<ID3D12Device5> dxr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxr.put()))))
            return {};

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = uint32_t(bottoms.size());

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        dxr->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (!info.ResultDataMaxSizeInBytes ||
            info.ResultDataMaxSizeInBytes == UINT64_MAX ||
            !info.ScratchDataSizeInBytes ||
            info.ScratchDataSizeInBytes == UINT64_MAX)
            return {};

        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto resultDesc = CD3DX12_RESOURCE_DESC::Buffer(
            info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto scratchDesc = CD3DX12_RESOURCE_DESC::Buffer(
            info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto instanceDesc = CD3DX12_RESOURCE_DESC::Buffer(
            uint64_t(bottoms.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        const auto resultBytes = device->GetResourceAllocationInfo(
            0, 1, &resultDesc).SizeInBytes;
        const auto scratchBytes = device->GetResourceAllocationInfo(
            0, 1, &scratchDesc).SizeInBytes;
        const auto instanceBytes = device->GetResourceAllocationInfo(
            0, 1, &instanceDesc).SizeInBytes;
        if (!resultBytes || resultBytes == UINT64_MAX ||
            !scratchBytes || scratchBytes == UINT64_MAX ||
            !instanceBytes || instanceBytes == UINT64_MAX ||
            resultBytes > UINT64_MAX - scratchBytes ||
            resultBytes + scratchBytes > UINT64_MAX - instanceBytes)
            return {};

        // CurrentUsage already includes prior allocations, so the budget
        // tracks only this chain's known live resources once.
        uint64_t known = 0;
        for (const auto& part : bottoms) {
            uint64_t partBytes = part->allocationBytes;
            if (part->rigidSource) {
                if (part->rigidSource->allocationBytes > UINT64_MAX - partBytes)
                    return {};
                partBytes += part->rigidSource->allocationBytes;
            } else {
                if (part->deformation->allocationBytes > UINT64_MAX - partBytes)
                    return {};
                partBytes += part->deformation->allocationBytes;
                if (part->deformation->input) {
                    const auto staged = part->deformation->input->residentBytes;
                    if (staged > UINT64_MAX - partBytes) return {};
                    partBytes += staged;
                }
            }
            if (partBytes > UINT64_MAX - known) return {};
            known += partBytes;
        }
        VideoMemoryBudget memory;
        const auto budget = memory.Query(device, known, 0);
        const uint64_t required = resultBytes + scratchBytes + instanceBytes;
        if (!budget.dxgiAvailable || known > budget.effectiveBytes ||
            required > budget.effectiveBytes - known)
            return {};

        auto tlas = std::make_shared<CharacterIsolatedTLAS>();
        tlas->bottoms = std::move(bottoms);
        tlas->instanceTransforms = std::move(instanceTransforms);
        tlas->bottom = tlas->bottoms.front();
        tlas->allocationBytes = resultBytes;
        tlas->scratchBytes = scratchBytes;
        tlas->poseFrame = observationFrame ? observationFrame :
            tlas->bottom->poseFrame;
        tlas->actorFormID = tlas->bottom->actorFormID;
        tlas->partOrdinal = tlas->bottom->partOrdinal;
        auto submission = std::make_shared<CharacterTLASSubmission>();
        submission->result = tlas;

        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &resultDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            nullptr, IID_PPV_ARGS(tlas->acceleration.put()))) ||
            FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                &scratchDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(submission->scratch.put()))) ||
            FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                &instanceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(submission->instanceUpload.put()))))
            throw std::runtime_error("Isolated character TLAS allocation failed");

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances(
            tlas->bottoms.size());
        for (size_t i = 0; i < instances.size(); ++i) {
            auto& instance = instances[i];
            std::memcpy(instance.Transform,
                tlas->instanceTransforms[i].data(),
                sizeof(instance.Transform));
            instance.InstanceID = uint32_t(i);
            // Dedicated per-instance bits let the diagnostic shader prove
            // that each sample intersects its intended BLAS. Visibility
            // shaders use 0xff to include all eligible parts.
            instance.InstanceMask = uint8_t(1u << i);
            instance.InstanceContributionToHitGroupIndex = 0;
            instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            instance.AccelerationStructure =
                tlas->bottoms[i]->acceleration->GetGPUVirtualAddress();
        }

        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        if (FAILED(submission->instanceUpload->Map(0, &noRead, &mapped)) || !mapped)
            throw std::runtime_error("Isolated TLAS instance mapping failed");
        std::memcpy(mapped, instances.data(),
            instances.size() * sizeof(instances.front()));
        submission->instanceUpload->Unmap(0, nullptr);
        inputs.InstanceDescs = submission->instanceUpload->GetGPUVirtualAddress();

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(submission->allocator.put()))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                submission->allocator.get(), nullptr,
                IID_PPV_ARGS(submission->commands.put()))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(tlas->completion.put()))))
            throw std::runtime_error("Isolated character TLAS commands unavailable");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = inputs;
        build.DestAccelerationStructureData =
            tlas->acceleration->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData =
            submission->scratch->GetGPUVirtualAddress();
        submission->commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(tlas->acceleration.get());
        submission->commands->ResourceBarrier(1, &barrier);
        if (FAILED(submission->commands->Close()))
            throw std::runtime_error("Isolated character TLAS command list invalid");

        ID3D12CommandList* commands[]{ submission->commands.get() };
        queue->ExecuteCommandLists(1, commands);
        tlas->fenceValue = 1;
        CharacterTLASRetirement::Retain(submission);
        if (FAILED(queue->Signal(tlas->completion.get(), tlas->fenceValue)))
            throw std::runtime_error("Isolated character TLAS signal failed");
        return tlas;
    }

    // Keep existing single-part callers and behavior unchanged.
    inline std::shared_ptr<const CharacterIsolatedTLAS> BuildCharacterIsolatedTLAS(
        ID3D12Device* device, ID3D12CommandQueue* queue,
        std::shared_ptr<const CharacterIsolatedBLAS> bottom)
    {
        if (!bottom) return {};
        return BuildCharacterMultiPartTLAS(device, queue,
            { std::move(bottom) });
    }
}
