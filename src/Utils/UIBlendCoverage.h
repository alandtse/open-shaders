#pragma once

#include <d3d11.h>

namespace Util::UIBlend
{
	/**
	 * @brief Rewrites the alpha blend of each render target so a separate UI layer accumulates coverage
	 * for premultiplied compositing.
	 * @param desc Blend description, modified in place.
	 * @return True if any render target's alpha blend changed.
	 */
	inline bool PatchCoverageAlpha(D3D11_BLEND_DESC& desc)
	{
		const int slotCount = desc.IndependentBlendEnable ? D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT : 1;
		bool changed = false;
		for (int i = 0; i < slotCount; i++) {
			auto& rt = desc.RenderTarget[i];
			// Destination-only blends erase coverage; alpha correction would make the cleared UI opaque.
			if (!rt.BlendEnable || rt.SrcBlend == D3D11_BLEND_ZERO)
				continue;

			// An additive draw covers nothing, so any coverage it adds darkens the scene under the UI.
			const bool additive = rt.DestBlend == D3D11_BLEND_ONE && rt.BlendOp == D3D11_BLEND_OP_ADD;
			const D3D11_BLEND srcAlpha = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
			const D3D11_BLEND destAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;

			if (rt.SrcBlendAlpha != srcAlpha ||
				rt.DestBlendAlpha != destAlpha ||
				rt.BlendOpAlpha != D3D11_BLEND_OP_ADD) {
				rt.SrcBlendAlpha = srcAlpha;
				rt.DestBlendAlpha = destAlpha;
				rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
				changed = true;
			}
		}
		return changed;
	}
}
