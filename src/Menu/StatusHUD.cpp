#include "StatusHUD.h"

#include <imgui.h>

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

	// HUD-mode helper client: always composited, never focused. The SDK's
	// RenderHud owns the private ImGui context + DX11 backend, the supersample
	// DPI, the per-frame window-position re-honoring, and the clear-once — so the
	// client just supplies its style and the draw calls.
	API::Client g_hud;
	bool g_connected = false;
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
		if (!globals::d3d::device || !globals::d3d::context || !menu)
			return;

		if (!g_connected) {
			if (!g_hud.Connect("CommunityShaders.HUD", Plugin::VERSION.string().c_str(),
					API::kClientFlag_HUDMode))
				return;
			// Load CS's fonts + theme into the HUD context (once) so the overlays
			// look identical to the desktop.
			g_hud.SetHudStyleCallback([menu]() { ThemeManager::SetupImGuiStyle(*menu); });
			g_connected = true;
		}

		// The focused menu panel presents (and interacts with) the overlays when
		// it's up; the HUD only mirrors them when the menu isn't shown, so they're
		// never drawn twice and interaction stays on the menu renderer.
		const bool menuShown = ImGuiVRHelperClient::HelperRequestsRender() || menu->IsEnabled;

		// Logical size = CS's main-context DisplaySize (read before RenderHud
		// switches contexts) so overlay positions match the flat screen.
		const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

		g_hud.RenderHud(globals::d3d::device, globals::d3d::context, displaySize, [menuShown]() {
			if (menuShown)
				return;  // menu renders the overlays itself; HUD stands down (panel clears)
			OverlayRenderer::RenderShaderCompilationStatus(
				[](std::vector<InputCombo> keys) -> const char* {
					static std::string cache;
					cache = Util::Input::KeyIdToString(keys);
					return cache.c_str();
				});
			OverlayRenderer::RenderShaderBlockingStatus();
			OverlayRenderer::RenderFeatureOverlays();
		});
	}

	void Shutdown()
	{
		g_hud.ShutdownHud();
	}
}
