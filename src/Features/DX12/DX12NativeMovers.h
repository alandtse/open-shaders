#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

namespace RE
{
	class BSGeometry;
}

namespace DX12Native
{
	/** @brief A rigid caster that moved, as drawn this frame: engine buffers plus its world transform. */
	struct MoverInstance
	{
		const void* vertices = nullptr;
		const void* indices = nullptr;
		uint32_t stride = 0, vertexCount = 0, triangleCount = 0;
		std::array<float, 12> transform{};
	};

	/** @brief Collects rigid movers from SCM caster walks and records their BLASes and a per-frame TLAS. */
	class MoverScene
	{
	public:
		static constexpr uint32_t kMaxInstances = 512;
		static constexpr uint32_t kMaxNewMeshesPerFrame = 8;

		/**
		 * @brief Render thread, per caster draw: records a rigid mover once per frame.
		 * @return True when its BLAS is ready, so raster may drop it for a light the trace covers.
		 */
		bool Observe(RE::BSGeometry& a_geometry, uint32_t a_frame);
		/** @brief This frame's movers; valid until the next frame's first Observe. */
		std::span<const MoverInstance> Instances(uint32_t a_frame) const;

		/**
		 * @brief Records new mesh BLASes and this frame's mover TLAS into a_commands.
		 * @param a_retireValue Fence value the submission will signal; resources stay alive until it passes.
		 * @return GPU address of the mover TLAS, or 0 when there is nothing to trace.
		 */
		D3D12_GPU_VIRTUAL_ADDRESS Record(ID3D12Device5* a_device, ID3D12GraphicsCommandList4* a_commands,
			ID3D12Fence* a_completion, uint64_t a_retireValue, uint32_t a_frame);
		/** @brief Drops BLASes recorded for a submission that never reached the queue. */
		void Discard(uint64_t a_retireValue);
		void Reset();
		/** @brief Instances in the last recorded mover TLAS, for diagnostics. */
		uint32_t LastTraced() const { return lastTraced.load(std::memory_order_relaxed); }

	private:
		struct Mesh
		{
			winrt::com_ptr<ID3D12Resource> blas;
			uint64_t lastUse = 0, built = 0;
			uint32_t lastFrame = 0;
		};
		struct Retired
		{
			winrt::com_ptr<ID3D12Resource> resource;
			uint64_t fence = 0;
		};
		struct FrameTop
		{
			winrt::com_ptr<ID3D12Resource> tlas, instances, scratch;
			uint64_t fence = 0;
		};
		static uint64_t MeshKey(const MoverInstance& a_instance);
		bool BuildMesh(ID3D12Device5* a_device, ID3D12GraphicsCommandList4* a_commands, const MoverInstance& a_instance,
			uint64_t a_retireValue, Mesh& a_mesh);
		void Retire(winrt::com_ptr<ID3D12Resource> a_resource, uint64_t a_fence);

		std::vector<MoverInstance> instances;
		std::vector<const RE::BSGeometry*> seen;  // Dedupes a mover drawn for several lights.
		uint32_t instanceFrame = UINT32_MAX;
		std::unordered_map<uint64_t, Mesh> meshes;
		std::vector<Retired> retired;
		std::array<FrameTop, 4> tops;
		uint32_t nextTop = 0;
		std::atomic<uint32_t> lastTraced{ 0 };
	};
}
