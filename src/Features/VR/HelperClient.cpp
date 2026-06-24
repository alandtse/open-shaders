// ImGuiVRHelper client integration for the VR feature.
//
// The VR overlay / menu / input / HUD is provided by the standalone ImGuiVRHelper
// SKSE plugin; the VR feature is its client. This file implements the VR:: methods
// declared in VR.h that wrap the client SDK with CS-specific glue: mapping CS
// keybind settings to combos, persisting rebinds, driving Menu::IsEnabled, and the
// always-on status HUD. Two helper clients: the focus-driven menu (CommunityShaders)
// and the always-on status HUD (CommunityShaders.HUD).

#include "Features/VR.h"

#include <imgui.h>

#include "Globals.h"
#include "ImGuiVRHelperClientSDK.h"
#include "Menu.h"
#include "Menu/OverlayRenderer.h"
#include "Menu/ThemeManager.h"
#include "State.h"
#include "Utils/Input.h"

namespace
{
	namespace API = ImGuiVRHelperPluginAPI;

	// The SDK owns the handshake, per-frame controller snapshot, focus sync, input
	// pump, and panel blit. We keep only CS glue. One VR feature singleton, so a
	// single client of each kind is file-local here.
	API::Client g_client;  // focus-driven menu client
	API::Client g_hud;     // always-on HUD-mode client (status overlays + welcome)
	bool g_hudConnected = false;

	bool g_combosRegistered = false;
	// Main menu open/close are owned by the helper now; CS keeps only its secondary
	// in-menu overlay toggle.
	API::ComboId g_overlayOpenCombo = 0;
	API::ComboId g_overlayCloseCombo = 0;

	// CS's ButtonCombo (Utils/Input.h) and the helper's API::InputCombo are
	// layout-compatible but distinct types; convert element-by-element (packed
	// device/key round-trips through uint32_t).
	std::vector<API::InputCombo> ToApi(const std::vector<ButtonCombo>& binds)
	{
		std::vector<API::InputCombo> out;
		out.reserve(binds.size());
		for (const auto& b : binds)
			out.emplace_back(static_cast<API::InputDeviceType>(static_cast<uint32_t>(b.GetDevice())), b.GetKey());
		return out;
	}

	// Build an on-rebind callback that writes the new chord back into the given CS
	// settings vector (a stable-address member of the VR settings singleton) and
	// persists settings to disk.
	API::Client::RebindCallback MakePersist(std::vector<ButtonCombo>& target)
	{
		return [&target](const API::InputCombo* keys, std::size_t n) {
			target.clear();
			target.reserve(n);
			for (std::size_t i = 0; i < n; ++i)
				target.emplace_back(static_cast<InputDeviceType>(static_cast<uint32_t>(keys[i].GetDevice())), keys[i].GetKey());
			if (globals::state)
				globals::state->Save(State::ConfigMode::USER);
			logger::info("ImGuiVRHelper: persisted VR combo rebind ({} keys)", n);
		};
	}

	// Register CS's secondary in-menu overlay combos with the helper once, on the
	// first UpdateHelper() (settings are loaded by then). Main menu open/close are
	// owned by the helper (it focuses the last-opened overlay), so CS no longer
	// registers those; it renders when the helper grants focus (RendersOnFocus).
	void EnsureCombosRegistered()
	{
		if (g_combosRegistered)
			return;
		auto& s = globals::features::vr.settings;
		// A default-constructed Settings carries the factory defaults from VR.h's
		// member initializers — pass them so the bindings table's Reset works.
		const VR::Settings d{};
		g_overlayOpenCombo = g_client.AddCombo("Open overlay", ToApi(s.VROverlayOpenKeys),
			MakePersist(s.VROverlayOpenKeys), ToApi(d.VROverlayOpenKeys));
		g_overlayCloseCombo = g_client.AddCombo("Close overlay", ToApi(s.VROverlayCloseKeys),
			MakePersist(s.VROverlayCloseKeys), ToApi(d.VROverlayCloseKeys));
		g_combosRegistered = true;
		logger::info("ImGuiVRHelper: registered VR overlay combos (overlayOpen={}, overlayClose={})",
			g_overlayOpenCombo, g_overlayCloseCombo);
	}
}

void VR::ConnectHelper()
{
	// Called from PostPostLoad (kPostPostLoad) — after globals::ReInit() has cached
	// the VR flag, and after the helper has registered its handshake listener.
	if (!globals::game::isVR)
		return;

	// kClientFlag_RendersOnFocus: when the helper routes in-scene focus to us, we
	// render the menu into the panel RTV regardless of Menu::IsEnabled.
	if (!g_client.Connect("CommunityShaders", Plugin::VERSION.string().c_str(),
			API::kClientFlag_RendersOnFocus)) {
		logger::info("ImGuiVRHelper not detected; VR menus will only render on the desktop monitor");
		return;
	}
	logger::info("ImGuiVRHelper handshake successful (build {}), client_id={}",
		g_client.Helper()->GetBuildNumber(), g_client.Id());
}

bool VR::IsHelperRegistered() const { return g_client.IsConnected(); }
bool VR::HelperRequestsRender() const { return g_client.HasFocus(); }

// ctx omitted: the SDK derives the immediate context from the panel RTV's device.
void VR::RenderHelperToPanel() { g_client.RenderToPanel(); }

void VR::FeedHelperEvent(uint32_t device, uint32_t key, bool pressed, float stickX, float stickY)
{
	g_client.FeedVREvent(device, key, pressed, stickX, stickY);
}

void VR::DrawHelperBindingsTable() { g_client.DrawBindingsTable(); }

void VR::UpdateHelper()
{
	if (!g_client.IsConnected())
		return;

	EnsureCombosRegistered();

	auto* menu = globals::menu;
	if (!menu) {
		g_client.PumpInput(false);
		return;
	}

	// Reconcile the menu-open flag with helper focus (the single source of truth for
	// VR visibility): CS toggling its own menu drives focus; the helper changing
	// focus (open/close combo, cycle, swap) drives IsEnabled.
	g_client.ReconcileFocus(menu->IsEnabled);
	const bool focused = g_client.HasFocus();

	if (focused) {
		// CS's secondary in-menu overlay stays CS-owned.
		if (g_client.Fired(g_overlayOpenCombo))
			menu->overlayVisible = true;
		if (g_client.Fired(g_overlayCloseCombo))
			menu->overlayVisible = false;
	} else if (g_overlayOpenCombo || g_overlayCloseCombo) {
		// Drain latches while closed so they don't fire stale on reopen.
		g_client.Fired(g_overlayOpenCombo);
		g_client.Fired(g_overlayCloseCombo);
	}

	g_client.PumpInput(focused, settings.mouseDeadzone);
}

void VR::RenderStatusHud()
{
	// Always-on status overlays (shader-compile progress, etc.) are invisible in VR
	// unless the menu is focused, because CS renders its whole frame into one
	// focus-driven panel. Mirror them into a dedicated HUD-mode client with its own
	// ImGui context (a shared context would clear the menu's input state and break
	// interaction). No-op on desktop or when the helper isn't registered.
	if (!globals::game::isVR || !g_client.IsConnected())
		return;

	auto* menu = Menu::GetSingleton();
	if (!globals::d3d::device || !globals::d3d::context || !menu)
		return;

	if (!g_hudConnected) {
		if (!g_hud.Connect("CommunityShaders.HUD", Plugin::VERSION.string().c_str(),
				API::kClientFlag_HUDMode))
			return;
		// Load CS's fonts + theme into the HUD context (once) so the overlays look
		// identical to the desktop.
		g_hud.SetHudStyleCallback([menu]() { ThemeManager::SetupImGuiStyle(*menu); });
		g_hudConnected = true;
	}

	// The focused menu panel presents the overlays when it's up; the HUD only mirrors
	// them when the menu isn't shown, so they're never drawn twice and interaction
	// stays on the menu renderer.
	const bool menuShown = g_client.HasFocus() || menu->IsEnabled;

	// Logical size = CS's main-context DisplaySize (read before RenderHud switches
	// contexts) so overlay positions match the flat screen.
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
