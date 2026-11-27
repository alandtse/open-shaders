#include "DX12NativeMovers.h"
#include "Globals.h"
#include "ScenePrepEngineBridge.h"
#include "ScenePrepMath.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <directx/d3dx12.h>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kMaxMoverVertices = 65535;
		// Meshes unseen this long are evicted once their last trace has retired.
		constexpr uint32_t kMeshRetentionFrames = 300;

		winrt::com_ptr<ID3D12Resource> Buffer(ID3D12Device5* a_device, uint64_t a_size, D3D12_HEAP_TYPE a_heap,
			D3D12_RESOURCE_STATES a_state, const wchar_t* a_name)
		{
			const auto heap = CD3DX12_HEAP_PROPERTIES(a_heap);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_size,
				a_heap == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
			winrt::com_ptr<ID3D12Resource> result;
			DX::ThrowIfFailed(a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, a_state, nullptr,
				IID_PPV_ARGS(result.put())));
			result->SetName(a_name);
			return result;
		}

		void Upload(ID3D12Resource* a_resource, const void* a_bytes, size_t a_count, size_t a_offset = 0)
		{
			const D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			DX::ThrowIfFailed(a_resource->Map(0, &noRead, &mapped));
			std::memcpy(static_cast<uint8_t*>(mapped) + a_offset, a_bytes, a_count);
			a_resource->Unmap(0, nullptr);
		}
	}

	uint64_t MoverScene::MeshKey(const MoverInstance& a_instance)
	{
		// Instances of one mesh share engine buffers, so they share one BLAS.
		return std::bit_cast<uint64_t>(a_instance.vertices) ^ (uint64_t(a_instance.triangleCount) << 48) ^
		       (std::bit_cast<uint64_t>(a_instance.indices) << 1);
	}

	bool MoverScene::Observe(RE::BSGeometry& a_geometry, uint32_t a_frame)
	{
		if (a_frame != instanceFrame) {
			instances.clear();
			seen.clear();
			instances.reserve(kMaxInstances);
			seen.reserve(kMaxInstances);
			instanceFrame = a_frame;
		}
		auto* tri = a_geometry.AsTriShape();
		if (!tri || a_geometry.AsDynamicTriShape())
			return false;
		const auto& runtime = a_geometry.GetGeometryRuntimeData();
		if (runtime.skinInstance || runtime.alphaProperty || !runtime.rendererData ||
			!runtime.rendererData->rawVertexData || !runtime.rendererData->rawIndexData)
			return false;
		const auto* property = runtime.shaderProperty.get();
		if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
			return false;
		const auto desc = runtime.vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
			return false;
		const auto counts = tri->GetTrishapeRuntimeData();
		const uint32_t stride = DX12::VertexStrideBytes(std::bit_cast<uint64_t>(desc));
		if (stride < 16 || stride > 256 || !counts.vertexCount || !counts.triangleCount ||
			counts.vertexCount > kMaxMoverVertices || counts.triangleCount > kMaxMoverVertices)
			return false;

		const MoverInstance instance{ runtime.rendererData->rawVertexData, runtime.rendererData->rawIndexData,
			stride, counts.vertexCount, counts.triangleCount,
			DX12::TransformToRows3x4(DX12::FromNiTransform(a_geometry.world)) };
		if (!std::ranges::contains(seen, &a_geometry)) {
			if (instances.size() >= kMaxInstances)
				return false;
			seen.push_back(&a_geometry);
			instances.push_back(instance);
		}
		const auto mesh = meshes.find(MeshKey(instance));
		return mesh != meshes.end() && mesh->second.blas;
	}

	std::span<const MoverInstance> MoverScene::Instances(uint32_t a_frame) const
	{
		return a_frame == instanceFrame ? std::span<const MoverInstance>(instances) : std::span<const MoverInstance>{};
	}

	void MoverScene::Retire(winrt::com_ptr<ID3D12Resource> a_resource, uint64_t a_fence)
	{
		if (a_resource)
			retired.push_back({ std::move(a_resource), a_fence });
	}

	bool MoverScene::BuildMesh(ID3D12Device5* a_device, ID3D12GraphicsCommandList4* a_commands,
		const MoverInstance& a_instance, uint64_t a_retireValue, Mesh& a_mesh)
	{
		std::vector<float3> positions;
		std::vector<uint32_t> indices;
		if (!DX12::DecodeRawMeshBuffers(static_cast<const uint8_t*>(a_instance.vertices), a_instance.vertexCount,
				a_instance.stride, static_cast<const uint16_t*>(a_instance.indices), 0, a_instance.triangleCount * 3u,
				positions, indices) ||
			positions.empty() || indices.empty())
			return false;
		const uint64_t vertexBytes = positions.size() * sizeof(float3);
		const uint64_t indexBytes = indices.size() * sizeof(uint32_t);
		auto source = Buffer(a_device, vertexBytes + indexBytes, D3D12_HEAP_TYPE_UPLOAD,
			D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::MoverGeometryUpload");
		Upload(source.get(), positions.data(), size_t(vertexBytes));
		Upload(source.get(), indices.data(), size_t(indexBytes), size_t(vertexBytes));

		D3D12_RAYTRACING_GEOMETRY_DESC triangle{};
		triangle.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		triangle.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		triangle.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		triangle.Triangles.VertexCount = UINT(positions.size());
		triangle.Triangles.VertexBuffer = { source->GetGPUVirtualAddress(), sizeof(float3) };
		triangle.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		triangle.Triangles.IndexCount = UINT(indices.size());
		triangle.Triangles.IndexBuffer = source->GetGPUVirtualAddress() + vertexBytes;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = 1;
		inputs.pGeometryDescs = &triangle;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		a_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes)
			return false;
		auto blas = Buffer(a_device, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"DX12NativeScene::MoverBLAS");
		auto scratch = Buffer(a_device, info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DX12NativeScene::MoverBLASScratch");
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
		a_commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(blas.get());
		a_commands->ResourceBarrier(1, &barrier);
		Retire(std::move(source), a_retireValue);
		Retire(std::move(scratch), a_retireValue);
		a_mesh.blas = std::move(blas);
		return true;
	}

	D3D12_GPU_VIRTUAL_ADDRESS MoverScene::Record(ID3D12Device5* a_device, ID3D12GraphicsCommandList4* a_commands,
		ID3D12Fence* a_completion, uint64_t a_retireValue, uint32_t a_frame)
	{
		if (!a_device || !a_commands || !a_completion)
			return 0;
		const uint64_t completed = a_completion->GetCompletedValue();
		if (completed == UINT64_MAX)
			return 0;
		std::erase_if(retired, [&](const Retired& entry) { return entry.fence <= completed; });
		std::erase_if(meshes, [&](const auto& entry) {
			return a_frame - entry.second.lastFrame > kMeshRetentionFrames && entry.second.lastUse <= completed;
		});
		if (instanceFrame != a_frame || instances.empty())
			return 0;

		auto& top = tops[nextTop];
		if (top.fence > completed)
			return 0;

		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
		descs.reserve(instances.size());
		uint32_t built = 0;
		for (const auto& instance : instances) {
			auto& mesh = meshes[MeshKey(instance)];
			mesh.lastFrame = a_frame;
			if (!mesh.blas) {
				if (built >= kMaxNewMeshesPerFrame || !BuildMesh(a_device, a_commands, instance, a_retireValue, mesh))
					continue;
				++built;
			}
			mesh.lastUse = a_retireValue;
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			std::memcpy(desc.Transform, instance.transform.data(), sizeof(desc.Transform));
			desc.InstanceID = UINT(descs.size());
			desc.InstanceMask = 0xFF;
			desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
			desc.AccelerationStructure = mesh.blas->GetGPUVirtualAddress();
			descs.push_back(desc);
		}
		lastTraced.store(uint32_t(descs.size()), std::memory_order_relaxed);
		if (descs.empty())
			return 0;

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		if (!top.tlas) {
			// Sized once for the instance cap, so a busy frame never reallocates.
			inputs.NumDescs = kMaxInstances;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			a_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes)
				return 0;
			top.tlas = Buffer(a_device, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"DX12NativeScene::MoverTLAS");
			top.scratch = Buffer(a_device, info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DX12NativeScene::MoverTLASScratch");
			top.instances = Buffer(a_device, uint64_t(kMaxInstances) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
				D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::MoverInstances");
		}
		Upload(top.instances.get(), descs.data(), descs.size() * sizeof(descs.front()));
		inputs.NumDescs = UINT(descs.size());
		inputs.InstanceDescs = top.instances->GetGPUVirtualAddress();
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = top.tlas->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = top.scratch->GetGPUVirtualAddress();
		a_commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(top.tlas.get());
		a_commands->ResourceBarrier(1, &barrier);
		top.fence = a_retireValue;
		nextTop = (nextTop + 1) % uint32_t(tops.size());
		return top.tlas->GetGPUVirtualAddress();
	}

	void MoverScene::Reset()
	{
		instances.clear();
		seen.clear();
		instanceFrame = UINT32_MAX;
	}
}
