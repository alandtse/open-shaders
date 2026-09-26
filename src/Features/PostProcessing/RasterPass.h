#pragma once

#include "Utils/D3D.h"

#include <d3d11.h>

#include <initializer_list>

struct ID3D11DeviceContext;
struct ID3D11VertexShader;
struct ID3D11PixelShader;

namespace PostProcessingRaster
{
	/** Restores the D3D11 state changed by fullscreen post-processing raster passes. */
	struct RasterPass
	{
		/** Saves the touched state and binds fullscreen-triangle state with shared pixel constants. */
		explicit RasterPass(ID3D11DeviceContext* a_context);

		/** Restores saved pipeline state and releases its references. */
		~RasterPass() = default;

		RasterPass(const RasterPass&) = delete;
		RasterPass& operator=(const RasterPass&) = delete;

		/** Binds render targets and a viewport matching the pass resolution. */
		void SetTargets(std::initializer_list<ID3D11RenderTargetView*> a_rtvs, float a_width, float a_height);

		/** Binds the fullscreen vertex shader and the pass pixel shader. */
		void SetShaders(ID3D11VertexShader* a_vs, ID3D11PixelShader* a_ps);

		/** Selects blending for subsequent draws. */
		void SetBlendState(ID3D11BlendState* a_blend, const float (&a_blendFactor)[4], UINT a_sampleMask = 0xFFFFFFFFu);

		/** Draws one fullscreen triangle without vertex buffers. */
		void Draw();

	private:
		static constexpr UINT kPSSRVCount = 6;
		static constexpr UINT kPSCBCount = 7;
		static constexpr UINT kSharedCBStart = 5;

		ID3D11DeviceContext* context;
		Util::FullscreenPassScope savedState;
	};
}
