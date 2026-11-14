#pragma once

#include "Features/DX12NativeScene/NativeCharacterBLASPreflight.h"

#include <algorithm>
#include <cstdint>
#include <directx/d3dx12.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
	// Experimental result. No TLAS instances, descriptor bindings or native
	// character shadow ownership ever consume this resource.
	struct CharacterIsolatedBLAS
	{
		winrt::com_ptr<ID3D12Device> device;
		std::shared_ptr<const CharacterDeformedResult> deformation;
		// Rigid geometry uses the native GPU source directly; its world-space
		// transform belongs to the TLAS instance, not to this reusable BLAS.
		std::shared_ptr<const CharacterStagedMesh> rigidSource;
		// Pose-specific validation travels with this immutable GPU generation.
		std::optional<CharacterDeformValidation> validation;
		winrt::com_ptr<ID3D12Resource> acceleration;
		winrt::com_ptr<ID3D12Fence> completion;
		uint64_t completionValue = 0, allocationBytes = 0, scratchBytes = 0;
		uint64_t sourceToken = 0;
		uint32_t actorFormID = 0, partOrdinal = 0, poseFrame = 0;
		uint32_t vertices = 0, indices = 0;
		uint64_t indexOffset = 0;
		bool refit = false;

		bool Ready() const
		{
			if (!completion || !completionValue)
				return false;
			const auto done = completion->GetCompletedValue();
			return done != UINT64_MAX && done >= completionValue;
		}
	};

	// A GPU submission retains the previous BLAS (on updates), the command
	// allocator, scratch and the input until its fence signals. Once complete,
	// this record can disappear while the published BLAS remains immutable.
	struct CharacterBLASSubmission
	{
		std::shared_ptr<const CharacterIsolatedBLAS> result;
		std::shared_ptr<const CharacterIsolatedBLAS> previous;
		winrt::com_ptr<ID3D12Resource> scratch;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
	};

	struct CharacterBLASRetirement
	{
		inline static std::mutex mutex;
		inline static std::vector<std::shared_ptr<CharacterBLASSubmission>> inFlight;

		static void PruneLocked()
		{
			std::erase_if(inFlight, [](const auto& submitted) {
				return !submitted || !submitted->result ||
				       submitted->result->Ready();
			});
		}

		static bool CanSubmit()
		{
			std::scoped_lock lock(mutex);
			PruneLocked();
			return inFlight.size() < 4;
		}

		static void Retain(std::shared_ptr<CharacterBLASSubmission> submitted)
		{
			std::scoped_lock lock(mutex);
			inFlight.push_back(std::move(submitted));
		}

		static uint32_t Pending()
		{
			std::scoped_lock lock(mutex);
			PruneLocked();
			return uint32_t(inFlight.size());
		}
	};

	/** @brief Builds or updates one isolated third-person character BLAS.
     * Requires a fence-complete deformation and independent DXGI preflight.
     * No synchronous GPU wait, no production TLAS modification, no raster
     * replacement. Only the native CPU worker is allowed to call this.
     */
	inline std::shared_ptr<const CharacterIsolatedBLAS> BuildCharacterIsolatedBLAS(
		ID3D12Device* device, ID3D12CommandQueue* queue,
		const std::shared_ptr<const CharacterDeformedResult>& deformation,
		const CharacterBLASPreflight& planned,
		const CharacterDeformValidation& validation,
		const std::shared_ptr<const CharacterIsolatedBLAS>& previous)
	{
		if (!device || !queue || !deformation || !deformation->Ready() ||
			!deformation->positions || !deformation->input ||
			!deformation->input->pose ||
			!planned.dxrSupported || !planned.budgetAvailable ||
			planned.poseFrame != deformation->poseFrame ||
			!validation.sane || validation.poseFrame != deformation->poseFrame ||
			!CharacterBLASRetirement::CanSubmit())
			return {};

		std::shared_ptr<const CharacterStagedMesh> mesh;
		for (const auto& item : deformation->input->meshes) {
			if (item->skinned && item->actorFormID == deformation->actorFormID &&
				item->partOrdinal == deformation->partOrdinal &&
				item->sourceToken == deformation->input->pose->sourceToken &&
				item->vertices == deformation->vertices && item->indices >= 3 &&
				item->indices % 3u == 0u && item->vertexStride == 44u &&
				item->indexOffset % sizeof(uint32_t) == 0 && item->source) {
				mesh = item;
				break;
			}
		}
		if (!mesh)
			return {};
		const uint64_t sourceSize = mesh->source->GetDesc().Width;
		const uint64_t indexSize = uint64_t(mesh->indices) * sizeof(uint32_t);
		if (mesh->indexOffset > sourceSize ||
			indexSize > sourceSize - mesh->indexOffset)
			return {};

		winrt::com_ptr<ID3D12Device5> dxr;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxr.put()))))
			return {};

		D3D12_RAYTRACING_GEOMETRY_DESC geom{};
		geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geom.Triangles.VertexCount = mesh->vertices;
		geom.Triangles.VertexBuffer = {
			deformation->positions->GetGPUVirtualAddress(), sizeof(float) * 3
		};
		geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		geom.Triangles.IndexCount = mesh->indices;
		geom.Triangles.IndexBuffer =
			mesh->source->GetGPUVirtualAddress() + mesh->indexOffset;
		if (!geom.Triangles.VertexBuffer.StartAddress || !geom.Triangles.IndexBuffer)
			return {};

		const bool update = previous && previous->Ready() &&
		                    previous->device.get() == device &&
		                    previous->actorFormID == mesh->actorFormID &&
		                    previous->partOrdinal == mesh->partOrdinal &&
		                    previous->sourceToken == mesh->sourceToken &&
		                    previous->vertices == mesh->vertices &&
		                    previous->indices == mesh->indices &&
		                    previous->indexOffset == mesh->indexOffset &&
		                    previous->deformation && previous->deformation->input &&
		                    previous->deformation->input->pose &&
		                    previous->deformation->input->pose->sourceToken == mesh->sourceToken &&
		                    previous->deformation->poseFrame != deformation->poseFrame;

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
		               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = 1;
		inputs.pGeometryDescs = &geom;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		dxr->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		// PREBUILD reports both build and update scratch for the stable
		// ALLOW_UPDATE topology. PERFORM_UPDATE is a dispatch-time flag.
		if (update)
			inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
		const uint64_t resultSize = update ?
		                                previous->acceleration->GetDesc().Width :
		                                info.ResultDataMaxSizeInBytes;
		const uint64_t scratchSize = update ?
		                                 info.UpdateScratchDataSizeInBytes :
		                                 info.ScratchDataSizeInBytes;
		if (!resultSize || resultSize == UINT64_MAX ||
			!scratchSize || scratchSize == UINT64_MAX)
			return {};

		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		const auto resultDesc = CD3DX12_RESOURCE_DESC::Buffer(
			resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto scratchDesc = CD3DX12_RESOURCE_DESC::Buffer(
			scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto resultBytes = device->GetResourceAllocationInfo(0, 1, &resultDesc).SizeInBytes;
		const auto scratchBytes = device->GetResourceAllocationInfo(0, 1, &scratchDesc).SizeInBytes;
		if (!resultBytes || resultBytes == UINT64_MAX ||
			!scratchBytes || scratchBytes == UINT64_MAX ||
			scratchBytes > UINT64_MAX - resultBytes)
			return {};

		const uint64_t tracked = deformation->input->residentBytes +
		                         deformation->allocationBytes;
		if (tracked < deformation->allocationBytes)
			return {};
		const uint64_t previousResident = previous ? previous->allocationBytes : 0;
		if (previousResident > UINT64_MAX - tracked)
			return {};
		const uint64_t knownBytes = tracked + previousResident;
		VideoMemoryBudget memory;
		const auto budget = memory.Query(device, knownBytes, 0);
		if (!budget.dxgiAvailable || knownBytes > budget.effectiveBytes ||
			resultBytes + scratchBytes > budget.effectiveBytes - knownBytes)
			return {};

		auto output = std::make_shared<CharacterIsolatedBLAS>();
		output->device.copy_from(device);
		output->deformation = deformation;
		output->validation = validation;
		output->sourceToken = mesh->sourceToken;
		output->actorFormID = mesh->actorFormID;
		output->partOrdinal = mesh->partOrdinal;
		output->poseFrame = deformation->poseFrame;
		output->vertices = mesh->vertices;
		output->indices = mesh->indices;
		output->indexOffset = mesh->indexOffset;
		output->allocationBytes = resultBytes;
		output->scratchBytes = scratchBytes;
		output->refit = update;

		auto submission = std::make_shared<CharacterBLASSubmission>();
		submission->result = output;
		if (update)
			submission->previous = previous;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
				&resultDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
				nullptr, IID_PPV_ARGS(output->acceleration.put()))) ||
			FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
				&scratchDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				nullptr, IID_PPV_ARGS(submission->scratch.put()))))
			throw std::runtime_error("Native character BLAS allocation failed");
		output->acceleration->SetName(L"DX12NativeCharacter::IsolatedBLAS");
		submission->scratch->SetName(L"DX12NativeCharacter::BLASScratch");
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(submission->allocator.put()))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				submission->allocator.get(), nullptr,
				IID_PPV_ARGS(submission->commands.put()))) ||
			FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(output->completion.put()))))
			throw std::runtime_error("Native character BLAS command resources unavailable");

		auto* commands = submission->commands.get();
		const auto readyForAS = CD3DX12_RESOURCE_BARRIER::Transition(
			deformation->positions.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		commands->ResourceBarrier(1, &readyForAS);
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = output->acceleration->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = submission->scratch->GetGPUVirtualAddress();
		build.SourceAccelerationStructureData =
			update ? previous->acceleration->GetGPUVirtualAddress() : 0;
		commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto built = CD3DX12_RESOURCE_BARRIER::UAV(output->acceleration.get());
		commands->ResourceBarrier(1, &built);
		// Preserve the recorded state of the immutable compute output for
		// later isolated experiments. No production feature sees this buffer.
		const auto restore = CD3DX12_RESOURCE_BARRIER::Transition(
			deformation->positions.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commands->ResourceBarrier(1, &restore);
		if (FAILED(commands->Close()))
			throw std::runtime_error("Native character BLAS command list invalid");

		ID3D12CommandList* lists[]{ commands };
		queue->ExecuteCommandLists(1, lists);
		output->completionValue = 1;
		// The submission record owns scratch, command objects, input and
		// optional update source until the completion fence signals.
		CharacterBLASRetirement::Retain(submission);
		if (FAILED(queue->Signal(output->completion.get(), output->completionValue)))
			throw std::runtime_error("Native character BLAS fence signal failed");
		return output;
	}

	/** Static-topology DX12 BLAS for an opaque, CPU-backed rigid character
     * attachment. Transform changes never rebuild this bottom level; the
     * one-instance/multi-instance TLAS owns the actor-space transform.
     */
	inline std::shared_ptr<const CharacterIsolatedBLAS> BuildCharacterRigidBLAS(
		ID3D12Device* device, ID3D12CommandQueue* queue,
		std::shared_ptr<const CharacterStagedMesh> mesh)
	{
		if (!device || !queue || !mesh || mesh->skinned ||
			!mesh->source || !mesh->vertices || mesh->indices < 3u ||
			mesh->indices % 3u != 0u || mesh->vertexStride != 12u ||
			mesh->indexOffset % 4u != 0u ||
			!CharacterBLASRetirement::CanSubmit())
			return {};
		const uint64_t bytes = mesh->source->GetDesc().Width;
		const uint64_t indicesBytes = uint64_t(mesh->indices) * 4ull;
		if (mesh->indexOffset > bytes ||
			indicesBytes > bytes - mesh->indexOffset)
			return {};
		winrt::com_ptr<ID3D12Device5> dxr;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxr.put()))))
			return {};

		D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
		geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geometry.Triangles.VertexCount = mesh->vertices;
		geometry.Triangles.VertexBuffer.StartAddress =
			mesh->source->GetGPUVirtualAddress();
		geometry.Triangles.VertexBuffer.StrideInBytes = 12u;
		geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		geometry.Triangles.IndexCount = mesh->indices;
		geometry.Triangles.IndexBuffer =
			mesh->source->GetGPUVirtualAddress() + mesh->indexOffset;
		if (!geometry.Triangles.VertexBuffer.StartAddress ||
			!geometry.Triangles.IndexBuffer)
			return {};

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = 1;
		inputs.pGeometryDescs = &geometry;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		dxr->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		if (!info.ResultDataMaxSizeInBytes ||
			info.ResultDataMaxSizeInBytes == UINT64_MAX ||
			!info.ScratchDataSizeInBytes ||
			info.ScratchDataSizeInBytes == UINT64_MAX)
			return {};

		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		const auto resultDesc = CD3DX12_RESOURCE_DESC::Buffer(
			info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto scratchDesc = CD3DX12_RESOURCE_DESC::Buffer(
			info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const uint64_t resultBytes = device->GetResourceAllocationInfo(
											   0, 1, &resultDesc)
		                                 .SizeInBytes;
		const uint64_t scratchBytes = device->GetResourceAllocationInfo(
												0, 1, &scratchDesc)
		                                  .SizeInBytes;
		if (!resultBytes || resultBytes == UINT64_MAX ||
			!scratchBytes || scratchBytes == UINT64_MAX ||
			resultBytes > UINT64_MAX - scratchBytes)
			return {};
		VideoMemoryBudget policy;
		const auto budget = policy.Query(device, mesh->allocationBytes, 0);
		if (!budget.dxgiAvailable ||
			mesh->allocationBytes > budget.effectiveBytes ||
			resultBytes + scratchBytes >
				budget.effectiveBytes - mesh->allocationBytes)
			return {};

		auto output = std::make_shared<CharacterIsolatedBLAS>();
		output->device.copy_from(device);
		output->rigidSource = std::move(mesh);
		output->actorFormID = output->rigidSource->actorFormID;
		output->partOrdinal = output->rigidSource->partOrdinal;
		output->sourceToken = output->rigidSource->sourceToken;
		output->poseFrame = output->rigidSource->captureFrame;
		output->vertices = output->rigidSource->vertices;
		output->indices = output->rigidSource->indices;
		output->indexOffset = output->rigidSource->indexOffset;
		output->allocationBytes = resultBytes;
		output->scratchBytes = scratchBytes;

		auto submitted = std::make_shared<CharacterBLASSubmission>();
		submitted->result = output;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
				&resultDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
				nullptr, IID_PPV_ARGS(output->acceleration.put()))) ||
			FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
				&scratchDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				nullptr, IID_PPV_ARGS(submitted->scratch.put()))) ||
			FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(submitted->allocator.put()))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				submitted->allocator.get(), nullptr,
				IID_PPV_ARGS(submitted->commands.put()))) ||
			FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(output->completion.put()))))
			throw std::runtime_error("Native rigid attachment BLAS resources unavailable");

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = output->acceleration->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = submitted->scratch->GetGPUVirtualAddress();
		submitted->commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(output->acceleration.get());
		submitted->commands->ResourceBarrier(1, &barrier);
		if (FAILED(submitted->commands->Close()))
			throw std::runtime_error("Native rigid attachment BLAS command list invalid");

		ID3D12CommandList* lists[]{ submitted->commands.get() };
		queue->ExecuteCommandLists(1, lists);
		output->completionValue = 1;
		CharacterBLASRetirement::Retain(submitted);
		if (FAILED(queue->Signal(output->completion.get(), output->completionValue)))
			throw std::runtime_error("Native rigid attachment BLAS fence failed");
		return output;
	}
}
