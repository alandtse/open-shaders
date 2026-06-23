#include "StatusHUD.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_internal.h>

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

		// The helper supersamples HUD panels (see Overlay::Config::kHUDSupersample),
		// so the panel is larger than our logical DisplaySize. Keep DisplaySize at
		// the menu's logical size (so overlay positions match the desktop exactly)
		// and let DisplayFramebufferScale drive font rasterization up to the real
		// panel resolution — ImGui 1.92 bakes glyphs at this density, so text stays
		// crisp across the wide HUD quad instead of being upscaled.
		API::PanelHandle panel{};
		if (g_hud.Helper()->GetPanel(g_hud.Id(), &panel) && panel.width && panel.height &&
			displaySize.x > 0.0f && displaySize.y > 0.0f) {
			io.DisplayFramebufferScale =
				ImVec2(static_cast<float>(panel.width) / displaySize.x,
					static_cast<float>(panel.height) / displaySize.y);
		}

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();

		// The HUD is a passive mirror — its windows can never be dragged, so they
		// must re-honor their source position/size every frame. Re-allow the
		// positional Cond flags so FirstUseEver/Once behave like Always here. This
		// makes EVERY overlay track the menu with zero HUD-specific code in the
		// overlays themselves: a client just renders the same code into the HUD
		// context, and position-stickiness is the HUD context's concern, not the
		// overlay's. (Windows from prior frames; empty on the very first frame,
		// where FirstUseEver applies anyway.)
		for (ImGuiWindow* w : g_ctx->Windows) {
			w->SetWindowPosAllowFlags |=
				ImGuiCond_Always | ImGuiCond_Once | ImGuiCond_FirstUseEver | ImGuiCond_Appearing;
			w->SetWindowSizeAllowFlags |=
				ImGuiCond_Always | ImGuiCond_Once | ImGuiCond_FirstUseEver | ImGuiCond_Appearing;
		}

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
