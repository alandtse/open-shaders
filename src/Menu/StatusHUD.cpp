#include "StatusHUD.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "Globals.h"
#include "ImGuiVRHelperClient.h"
#include "ImGuiVRHelperClientSDK.h"
#include "Menu.h"
#include "Menu/OverlayRenderer.h"
#include "Menu/ThemeManager.h"
#include "Utils/Input.h"

namespace
{
	namespace API = ImGuiVRHelperPluginAPI;

	// Fallback panel size before the first real frame; overwritten each frame
	// with the host's DisplaySize so window positions match the desktop.
	constexpr float kPanelWidth = 1920.0f;
	constexpr float kPanelHeight = 1080.0f;

	// Dedicated, non-interactive ImGui context with its own DX11 backend. It
	// loads CS's fonts + style (ThemeManager::SetupImGuiStyle) so overlays look
	// identical to the desktop, and calls the SAME OverlayRenderer functions.
	// A separate context (not a second pass on the menu's context) avoids
	// clearing the menu's input state — the reason the earlier attempt was
	// reverted.
	ImGuiContext* g_ctx = nullptr;
	bool g_dx11Inited = false;

	// HUD-mode helper client: always composited, never focused.
	API::Client g_hud;
	bool g_connected = false;
	bool g_wasShowing = false;  // for the showing->hidden clear-once

	bool EnsureContext(Menu* menu)
	{
		if (g_ctx && g_dx11Inited)
			return true;
		if (!globals::d3d::device || !globals::d3d::context || !menu)
			return false;

		if (!g_ctx) {
			IMGUI_CHECKVERSION();
			g_ctx = ImGui::CreateContext();
			ImGui::SetCurrentContext(g_ctx);
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(kPanelWidth, kPanelHeight);
		}
		if (!g_dx11Inited) {
			ImGui::SetCurrentContext(g_ctx);
			if (!ImGui_ImplDX11_Init(globals::d3d::device, globals::d3d::context)) {
				logger::error("StatusHUD: ImGui_ImplDX11_Init failed");
				return false;
			}
			// Load CS's fonts + theme into this context so the HUD renders the
			// overlays with the exact look/positioning of the desktop overlay.
			ThemeManager::SetupImGuiStyle(*menu);
			g_dx11Inited = true;
		}
		return true;
	}

	void ClearPanel()
	{
		API::PanelHandle panel{};
		if (g_hud.IsConnected() && g_hud.Helper()->GetPanel(g_hud.Id(), &panel) && panel.rtv) {
			const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			globals::d3d::context->ClearRenderTargetView(panel.rtv, clear);
		}
	}
}

namespace StatusHUD
{
	bool OwnsStatusOverlays()
	{
		return globals::game::isVR && ImGuiVRHelperClient::IsRegistered();
	}

	void Render()
	{
		if (!OwnsStatusOverlays())
			return;

		auto* menu = Menu::GetSingleton();
		// The focused menu panel presents (and interacts with) the overlays when
		// it's up; the HUD only mirrors them when the menu isn't shown, so they're
		// never drawn twice in VR and interaction stays on the menu renderer.
		const bool menuShown = ImGuiVRHelperClient::HelperRequestsRender() || (menu && menu->IsEnabled);

		if (!g_connected) {
			if (!g_hud.Connect("CommunityShaders.HUD", Plugin::VERSION.string().c_str(),
					API::kClientFlag_HUDMode))
				return;
			g_connected = true;
		}

		// Capture the caller's context + DisplaySize BEFORE EnsureContext switches
		// the current context, and restore on every exit (leaving our own context
		// current would crash the host's ImGui_ImplWin32_NewFrame).
		ImGuiContext* prev = ImGui::GetCurrentContext();
		const ImVec2 displaySize = prev ? ImGui::GetIO().DisplaySize : ImVec2(kPanelWidth, kPanelHeight);

		if (!EnsureContext(menu)) {
			ImGui::SetCurrentContext(prev);
			return;
		}

		if (menuShown) {
			if (g_wasShowing) {
				ClearPanel();
				g_wasShowing = false;
			}
			ImGui::SetCurrentContext(prev);
			return;
		}

		ImGui::SetCurrentContext(g_ctx);
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = displaySize;
		io.DeltaTime = 1.0f / 60.0f;

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();

		// Identical code path: the real CS overlay renderers. Display only here —
		// input is handled by the menu renderer when the menu is open.
		OverlayRenderer::RenderShaderCompilationStatus(
			[](std::vector<InputCombo> keys) -> const char* {
				static std::string cache;
				cache = Util::Input::KeyIdToString(keys);
				return cache.c_str();
			});
		OverlayRenderer::RenderShaderBlockingStatus();
		OverlayRenderer::RenderFeatureOverlays();

		ImGui::Render();
		ImDrawData* drawData = ImGui::GetDrawData();
		if (drawData && drawData->CmdListsCount > 0) {
			g_hud.RenderToPanel(globals::d3d::context);
			g_wasShowing = true;
		} else if (g_wasShowing) {
			ClearPanel();
			g_wasShowing = false;
		}

		ImGui::SetCurrentContext(prev);
	}

	void Shutdown()
	{
		if (g_dx11Inited && g_ctx) {
			ImGui::SetCurrentContext(g_ctx);
			ImGui_ImplDX11_Shutdown();
			g_dx11Inited = false;
		}
		if (g_ctx) {
			ImGui::DestroyContext(g_ctx);
			g_ctx = nullptr;
		}
	}
}
