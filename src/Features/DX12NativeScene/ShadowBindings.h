#pragma once

#include "MaterialResources.h"

namespace DX12Native
{
	/** @brief Owns immutable geometry and material descriptor tables for shadow tracing. */
	struct ShadowBindings
	{
		static constexpr uint32_t kMaximumInstances = 8192;
		static constexpr uint32_t kFrameDescriptorCount = 3;
		static constexpr uint32_t kMaximumFrameSlots = 4;
		static constexpr uint32_t kGeometryDescriptorBase = kMaximumFrameSlots * kFrameDescriptorCount;
		static constexpr uint32_t kAttributeDescriptorBase = kGeometryDescriptorBase + kMaximumInstances;
		static constexpr uint32_t kTextureDescriptorBase = kAttributeDescriptorBase + kMaximumInstances;
		std::shared_ptr<const TopLevel> scene;
		std::shared_ptr<const MaterialResources> materials;
		winrt::com_ptr<ID3D12Resource> offsets;
		winrt::com_ptr<ID3D12DescriptorHeap> heap;
		uint64_t bytes = 0;
		/** @brief Creates the owned binding tables on a worker using a completed scene. */
		static std::shared_ptr<const ShadowBindings> Create(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene, uint64_t a_budget);
	};
	/** @brief Loads shadow material bindings asynchronously with revision-safe publication. */
	class ShadowBindingWorker
	{
	public:
		/** @brief Selects one scene, deferring a new load until an earlier worker retires. */
		void Request(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene, uint64_t a_budget);
		/** @brief Polls an owned worker without waiting and publishes only the selected revision. */
		void Advance();
		/** @brief Invalidates current bindings without waiting for the worker. */
		void Reset();
		/** @brief Retains the completed bindings for the selected scene. */
		std::shared_ptr<const ShadowBindings> Get() const { return published; }
		/** @brief Returns the most recent selected-scene load failure. */
		const std::string& Failure() const { return failure; }
	private:
		struct State;
		std::shared_ptr<State> pending;
		std::shared_ptr<const TopLevel> desired;
		std::shared_ptr<const ShadowBindings> published;
		GraphicsBackend backend;
		uint64_t revision = 0, budget = 0;
		bool attempted = false;
		std::string failure;
	};

}
