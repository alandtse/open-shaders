#pragma once

#include "Buffer.h"
#include <memory>
#include <string_view>
#include <type_traits>

namespace Util
{
	struct RuntimeResourceDiagnosticState;

	/** @brief Records allocations, release requests, destruction and DXGI usage for a resource transition. */
	class RuntimeResourceDiagnostics
	{
	public:
		RuntimeResourceDiagnostics(std::string_view feature, std::string_view operation);
		~RuntimeResourceDiagnostics();
		RuntimeResourceDiagnostics(const RuntimeResourceDiagnostics&) = delete;
		RuntimeResourceDiagnostics& operator=(const RuntimeResourceDiagnostics&) = delete;

	private:
		std::shared_ptr<RuntimeResourceDiagnosticState> state;
	};

	/** @brief Observes a newly named resource during an active lifecycle diagnostic scope. */
	void TrackRuntimeResourceAllocation(ID3D11DeviceChild* object);
	/** @brief Observes a resource before its bindings and ownership references are dropped. */
	void TrackRuntimeResourceRelease(ID3D11Resource* resource);
	/** @brief Reports delayed destruction and DXGI usage from the render-thread transition boundary. */
	void PollRuntimeResourceDiagnostics();

	/** @brief Removes context bindings retaining a feature-owned texture or buffer. */
	void UnbindRuntimeResource(ID3D11Resource* resource);

	/** @brief Unbinds an owned resource before its final application reference is released. */
	template <class T>
	void UnbindRuntimeObject(T* object)
	{
		if (!object)
			return;
		if constexpr (std::is_base_of_v<ID3D11Resource, T>) {
			UnbindRuntimeResource(object);
		} else if constexpr (std::is_base_of_v<ID3D11View, T>) {
			winrt::com_ptr<ID3D11Resource> resource;
			object->GetResource(resource.put());
			UnbindRuntimeResource(resource.get());
		} else if constexpr (requires { object->resource.get(); }) {
			UnbindRuntimeResource(object->resource.get());
		} else if constexpr (requires { object->CB(); }) {
			UnbindRuntimeResource(object->CB());
		} else if constexpr (requires { object->srvs; }) {
			for (auto& view : object->srvs)
				UnbindRuntimeObject(view.get());
		}
	}

	/** @brief Releases owned resources while retaining shader programs and settings. */
	template <class T>
	void ReleaseRuntimeResource(T& resource)
	{
		if constexpr (std::is_pointer_v<T>) {
			UnbindRuntimeObject(resource);
			if constexpr (std::is_base_of_v<IUnknown, std::remove_pointer_t<T>>) {
				if (resource)
					resource->Release();
			} else {
				delete resource;
			}
			resource = nullptr;
		} else if constexpr (requires { resource.get(); }) {
			UnbindRuntimeObject(resource.get());
			resource = nullptr;
		} else if constexpr (requires { resource.Get(); }) {
			UnbindRuntimeObject(resource.Get());
			resource.Reset();
		} else {
			for (auto& entry : resource)
				ReleaseRuntimeResource(entry);
		}
	}

	/** @brief Releases a group of feature-owned resources on the render thread. */
	template <class... T>
	void ReleaseRuntimeResources(T&... resources)
	{
		(ReleaseRuntimeResource(resources), ...);
	}
}
