// Separate TU for Util::Subrect::OpaquePreviewBlendCallback. Split from
// Subrect.cpp because this needs the plugin's d3d singletons (globals::d3d),
// and Subrect.cpp is also compiled standalone by the unit-test target
// (tests/cpp/CMakeLists.txt) which has no PCH and no D3D context to bind.
// Plugin builds pick this up automatically via the src/*.cpp GLOB_RECURSE.

#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/Subrect.h"

#include <PCH.h>
#include <d3d11.h>
#include <imgui.h>

namespace Util::Subrect
{
	void OpaquePreviewBlendCallback(const ImDrawList*, const ImDrawCmd*)
	{
		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context) {
			return;
		}

		static winrt::com_ptr<ID3D11BlendState> opaqueBlend;
		if (!opaqueBlend) {
			D3D11_BLEND_DESC desc{};
			desc.RenderTarget[0].BlendEnable = FALSE;
			desc.RenderTarget[0].RenderTargetWriteMask =
				D3D11_COLOR_WRITE_ENABLE_RED |
				D3D11_COLOR_WRITE_ENABLE_GREEN |
				D3D11_COLOR_WRITE_ENABLE_BLUE;
			if (FAILED(device->CreateBlendState(&desc, opaqueBlend.put()))) {
				return;
			}
			Util::SetResourceName(opaqueBlend.get(), "Subrect::OpaquePreviewBlend");
		}
		if (opaqueBlend) {
			context->OMSetBlendState(opaqueBlend.get(), nullptr, 0xFFFFFFFF);
		}
	}
}  // namespace Util::Subrect
