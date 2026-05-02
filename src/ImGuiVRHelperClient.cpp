#include "ImGuiVRHelperClient.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "Globals.h"
#include "ImGuiVRHelperAPI.h"

namespace
{
	ImGuiVRHelperPluginAPI::IImGuiVRHelperInterface001* g_helper = nullptr;
	uint32_t g_clientId = 0;

	void OnFrame(const ImGuiVRHelperPluginAPI::Frame*, void*)
	{
		// Phase 2: rendering is driven from SCS's render thread via
		// RenderToPanel() (called inside FinalizeImGuiFrame), not from
		// here. Phase 3 will move per-frame controller state ingestion
		// into this callback.
	}
}

namespace ImGuiVRHelperClient
{
	void Init()
	{
		if (!REL::Module::IsVR()) {
			return;
		}

		g_helper = ImGuiVRHelperPluginAPI::GetImGuiVRHelperInterface001();
		if (!g_helper) {
			logger::info("ImGuiVRHelper not detected; VR menus will only render on the desktop monitor");
			return;
		}

		g_clientId = g_helper->RegisterClient(
			"CommunityShaders",
			&OnFrame,
			nullptr,
			ImGuiVRHelperPluginAPI::kClientFlag_None);

		if (g_clientId == 0) {
			logger::warn("ImGuiVRHelper RegisterClient failed; VR menus will only render on the desktop monitor");
			g_helper = nullptr;
			return;
		}

		logger::info("ImGuiVRHelper handshake successful (build {}), client_id={}",
			g_helper->GetBuildNumber(), g_clientId);
	}

	bool IsRegistered()
	{
		return g_helper != nullptr && g_clientId != 0;
	}

	void RenderToPanel()
	{
		if (!IsRegistered()) {
			return;
		}

		ImGuiVRHelperPluginAPI::PanelHandle panel{};
		if (!g_helper->GetPanel(g_clientId, &panel) || panel.rtv == nullptr) {
			// Helper hasn't issued a panel yet (e.g. before its first
			// Submit-hook fires). Try again next frame.
			return;
		}

		ImDrawData* drawData = ImGui::GetDrawData();
		if (!drawData || !drawData->Valid || drawData->CmdListsCount <= 0) {
			return;
		}

		auto* context = globals::d3d::context;
		if (!context) {
			return;
		}

		// Save current render target / depth-stencil so the rest of the
		// frame's compositing chain isn't disturbed.
		ID3D11RenderTargetView* prevRTV = nullptr;
		ID3D11DepthStencilView* prevDSV = nullptr;
		context->OMGetRenderTargets(1, &prevRTV, &prevDSV);

		// Match the panel's full extents as the viewport. The helper
		// owns the texture's dimensions and may resize it between
		// frames, so we re-read each call rather than cache.
		D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(&prevViewportCount, prevViewports);

		D3D11_VIEWPORT panelViewport{};
		panelViewport.TopLeftX = 0.0f;
		panelViewport.TopLeftY = 0.0f;
		panelViewport.Width = static_cast<float>(panel.width);
		panelViewport.Height = static_cast<float>(panel.height);
		panelViewport.MinDepth = 0.0f;
		panelViewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &panelViewport);

		ID3D11RenderTargetView* panelRTV = panel.rtv;
		context->OMSetRenderTargets(1, &panelRTV, nullptr);

		// Clear to fully transparent so blank panel area passes through
		// to the underlying scene when the helper composites the quad.
		const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(panelRTV, clearColor);

		// ImDrawData remains valid between ImGui::Render() and the next
		// ImGui::NewFrame(). We rely on FinalizeImGuiFrame having just
		// called ImGui::Render() before invoking us.
		ImGui_ImplDX11_RenderDrawData(drawData);

		// Restore previous targets/viewports.
		context->OMSetRenderTargets(1, &prevRTV, prevDSV);
		if (prevViewportCount > 0) {
			context->RSSetViewports(prevViewportCount, prevViewports);
		}

		if (prevRTV) {
			prevRTV->Release();
		}
		if (prevDSV) {
			prevDSV->Release();
		}
	}

	void FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
		float thumbstick_x, float thumbstick_y)
	{
		if (!IsRegistered()) {
			return;
		}
		g_helper->FeedVREvent(device, key_code, pressed, thumbstick_x, thumbstick_y);
	}
}
