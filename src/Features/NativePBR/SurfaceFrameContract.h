#pragma once

#include "GpuMaterialAbi.h"
#include "MaterialSidecarContract.h"

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#include <winrt/base.h>

namespace NativePBR
{
	inline constexpr uint32_t kSurfaceSourceAbiVersion = 2;
	inline constexpr uint32_t kSurfaceFrameAbiVersion = 3;
	inline constexpr uint32_t kSurfaceFrameAttachmentCount = 8;
	inline constexpr uint32_t kSurfaceFrameBaseAttachments = 6;
	inline constexpr uint32_t kSurfaceTransportAttachmentCount = kSurfaceFrameAttachmentCount + 1;
	inline constexpr uint32_t kSurfaceTransportBaseAttachments = kSurfaceFrameBaseAttachments + 1;
	// Diagnostic surface transport may skip a frame rather than reserve five full-resolution copies.
	inline constexpr uint32_t kSurfaceFrameSlotCount = 3;
	inline constexpr std::array<DXGI_FORMAT, kSurfaceFrameAttachmentCount> kSurfaceFrameFormats{
		DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT
	};
	inline constexpr std::array<DXGI_FORMAT, kSurfaceTransportAttachmentCount> kSurfaceTransportFormats{
		DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UINT,
		DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT
	};
	static_assert(static_cast<size_t>(SidecarTarget::Count) == kSurfaceFrameAttachmentCount);
	static_assert(sizeof(GpuMaterialRecord) == 304 && sizeof(GpuDrawBinding) == 24);
	static_assert(kGpuTextureDescriptorCapacity == 512);

	/** @brief Rejects ambiguous frame-local ordinals before publishing native draw ownership. */
	inline bool ValidSurfaceDrawTable(std::span<const SidecarDraw> a_draws)
	{
		if (a_draws.empty() || a_draws.size() > kMaximumFrameDraws)
			return false;
		for (size_t index = 0; index < a_draws.size(); ++index)
			if (a_draws[index].ordinal != index + 1 || !a_draws[index].materialIdentity || !a_draws[index].materialRevision)
				return false;
		return true;
	}

	/** @brief GPU-visible, independently owned DX11 surface snapshot for screen-space NativePBR. */
	struct SurfaceFrame
	{
		std::array<winrt::com_ptr<ID3D12Resource>, kSurfaceTransportAttachmentCount> textures;
		std::array<D3D12_RESOURCE_STATES, kSurfaceTransportAttachmentCount> states{};
		std::shared_ptr<const std::vector<SidecarDraw>> draws;
		winrt::com_ptr<ID3D12Fence> readyFence;
		std::function<bool(ID3D12Fence*, uint64_t)> retainThrough;
		uint64_t readyValue = 0;
		uint32_t frame = 0, width = 0, height = 0;
		uint32_t abiVersion = kSurfaceFrameAbiVersion;
		bool nativeOwnedDraws = false, projectedAvailable = false;

		/** @brief True only after the producer's DX11 copy fence completes successfully. */
		bool ProducerComplete() const
		{
			if (!readyFence || !readyValue || readyValue == UINT64_MAX)
				return false;
			const auto completed = readyFence->GetCompletedValue();
			return completed != UINT64_MAX && completed >= readyValue;
		}
	};
}
