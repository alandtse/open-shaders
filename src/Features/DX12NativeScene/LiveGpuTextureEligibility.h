#pragma once

#include "Features/NativePBR/NativePBRCompiler.h"
#include <array>
#include <cstdint>
#include <d3d11.h>

namespace DX12Native
{
	/** @brief An observation, not a transferable/shared-resource guarantee. */
	enum class LiveGpuTextureRoute : uint8_t
	{
		Unobserved,
		UnsupportedShape,
		CopyCandidate,
		PotentialSharedHandle
	};

	/** @brief Classify only copied D3D11 descriptors; never borrow engine or GPU objects. */
	inline LiveGpuTextureRoute ClassifyLiveGpuTexture(const NativePBR::TextureResourceInfo& a_info)
	{
		if (!a_info.texture2D)
			return LiveGpuTextureRoute::Unobserved;
		if (!a_info.width || !a_info.height || a_info.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			a_info.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || !a_info.mipLevels ||
			a_info.mipLevels > 15 || a_info.arraySize != 1 || a_info.sampleCount != 1 ||
			a_info.format == DXGI_FORMAT_UNKNOWN)
			return LiveGpuTextureRoute::UnsupportedShape;
		// A shareable misc flag is only a *candidate* until a real NT handle,
		// adapter compatibility and cross-API synchronization are verified.
		return (a_info.miscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0 ?
		           LiveGpuTextureRoute::PotentialSharedHandle :
		           LiveGpuTextureRoute::CopyCandidate;
	}

	struct LiveGpuTextureRoutes
	{
		uint32_t unobserved = 0, unsupported = 0, copyCandidates = 0, sharedCandidates = 0;
		LiveGpuTextureRoutes& operator+=(const LiveGpuTextureRoutes& a_rhs)
		{
			unobserved += a_rhs.unobserved;
			unsupported += a_rhs.unsupported;
			copyCandidates += a_rhs.copyCandidates;
			sharedCandidates += a_rhs.sharedCandidates;
			return *this;
		}
	};

	inline LiveGpuTextureRoutes AssessLiveGpuRecipe(const NativePBR::Recipe& a_recipe)
	{
		LiveGpuTextureRoutes report;
		constexpr std::array roles{ NativePBR::TextureRole::BaseColor, NativePBR::TextureRole::Normal,
			NativePBR::TextureRole::Rmaos, NativePBR::TextureRole::Emission };
		for (const auto role : roles) {
			const auto& request = a_recipe.textures[static_cast<size_t>(role)];
			if (!request.required && request.assetPath.empty())
				continue;
			switch (ClassifyLiveGpuTexture(request.resource)) {
			case LiveGpuTextureRoute::Unobserved:
				++report.unobserved;
				break;
			case LiveGpuTextureRoute::UnsupportedShape:
				++report.unsupported;
				break;
			case LiveGpuTextureRoute::CopyCandidate:
				++report.copyCandidates;
				break;
			case LiveGpuTextureRoute::PotentialSharedHandle:
				++report.sharedCandidates;
				break;
			}
		}
		return report;
	}
}
