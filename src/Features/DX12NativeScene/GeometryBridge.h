#pragma once

#include "GraphicsBackend.h"
#include "Scene.h"
#include "GraphicsInterop.h"
#include <directx/d3dx12.h>

namespace DX12Native
{
	/** @brief Owns native DX12 geometry exported entirely on the GPU. */
	struct Geometry
	{
		std::shared_ptr<const Mesh> source;
		winrt::com_ptr<ID3D12Resource> buffer;
		uint64_t allocationBytes = 0;
		// Content identity is resolved off-thread, never from engine pointers.
		std::string contentDigest;
		uint64_t indexOffset = 0;
		uint64_t validationOffset = 0;
		bool prepared = false;
	};

}
