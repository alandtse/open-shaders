#pragma once

#include "Globals.h"
#include "GraphicsInterop.h"
#include "PreparedScene.h"
#include "ShadowBindings.h"
#include "VideoMemoryBudget.h"
#include <span>

namespace DX12Native
{
	struct ShadowTraceControls
	{
		float reachMultiplier = 1.0f, falloffFraction = 0.1f, falloffExponent = 1.0f, emitterRadius = 1.0f;
		bool stochastic = true, distanceFade = false;
		uint32_t normalInfluence = 2;
		uint32_t traceDensityMode = 0;
	};
	/** @brief Retains one independently fenced prepared-scene shadow result. */
	struct ShadowFrame
	{
		std::shared_ptr<const TopLevel> scene;
		std::shared_ptr<const ShadowBindings> bindings;
		std::unique_ptr<WrappedResource> rays, mask, lights;
		DirectX::SimpleMath::Matrix rayProjInverse, rayViewInverse;
		winrt::com_ptr<ID3D12DescriptorHeap> rayPrepHeap;
		winrt::com_ptr<ID3D12RootSignature> rayPrepRoot;
		winrt::com_ptr<ID3D12PipelineState> rayPrepPipeline;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		GraphicsBackend retainedBackend;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> producerFence, readyFence;
		winrt::com_ptr<ID3D11Fence> producerFence11;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
		std::vector<PreparedPointLight> lightSnapshot;
		float3 cameraOrigin{};
		uint64_t readyValue = 0, producerValue = 0, bytes = 0;
		uint32_t frame = 0, width = 0, height = 0;
		bool completionCounted = false;
		ShadowTraceControls controls;
	};

	/** @brief Produces a separate early point-light visibility mask from prepared geometry. */
	class ShadowTrace
	{
	public:
		~ShadowTrace();
		/** @brief Attempts current-frame shadow tracing without waiting for bindings or GPU work. */
		void Dispatch(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene, std::shared_ptr<const ShadowBindings> a_bindings, std::span<const PreparedPointLight> a_lights,
			const globals::FrameBuffer& a_camera, std::array<uint32_t, 2> a_size, uint32_t a_frame, uint64_t a_budget, const ShadowTraceControls& a_controls);
		/** @brief Returns a matching mask; consumers must wait on its completion fence. */
		std::shared_ptr<const ShadowFrame> Get(uint32_t a_frame) const;
		/** @brief Retains an offered mask until the registered consumer fence completes. */
		bool RetainThrough(const std::shared_ptr<const ShadowFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
		/** @brief Invalidates publication while preserving in-flight resources. */
		void Reset();
		/** @brief Retires completed frames from older scenes without waiting. */
		bool RetireScenesExcept(const std::shared_ptr<const TopLevel>& a_scene);
		/** @brief Retries shadow shader and pipeline creation after a cache reload. */
		void ReloadShaders();
		/** @brief Returns the latest availability or failure reason. */
		const std::string& Status() const { return status; }
		/** @brief Reports submissions and fence completion without asserting visual consumption. */
		json GetDiagnostics() const;
	private:
		bool Initialize(const GraphicsBackend& a_backend);
		void CreatePipeline();
		void CreateRayPreparationPipeline();
		void PrepareRays(ShadowFrame& a_frame);
		void Record(ShadowFrame& a_frame, uint32_t a_descriptorBase);
		GraphicsBackend backend;
		SharedFence producer;
		winrt::com_ptr<ID3D12Fence> completion;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12RootSignature> rayPrepRoot;
		winrt::com_ptr<ID3D12PipelineState> rayPrepPipeline;
		std::array<std::shared_ptr<ShadowFrame>, ShadowBindings::kMaximumFrameSlots> frames;
		std::shared_ptr<ShadowFrame> current;
		uint64_t nextFence = 0, submissions = 0, completions = 0, busyDeferrals = 0;
		VideoMemoryBudget videoBudget;
		std::string status;
		bool failed = false;
	};
}
