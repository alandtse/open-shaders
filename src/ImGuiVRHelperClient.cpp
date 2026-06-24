#include "ImGuiVRHelperClient.h"

#include "Features/VR.h"  // VR::Settings + ButtonCombo (= InputCombo)
#include "Globals.h"
#include "ImGuiVRHelperClientSDK.h"
#include "State.h"

namespace
{
	namespace API = ImGuiVRHelperPluginAPI;

	// The SDK wrapper owns the handshake, per-frame controller snapshot, focus
	// sync, input pump, and panel blit — everything that used to be hand-rolled
	// in this file. We keep only CS-specific glue: mapping CS's keybind settings
	// to combos, persisting rebinds, and driving Menu::IsEnabled.
	API::Client g_client;

	bool g_combosRegistered = false;
	// Main menu open/close are owned by the helper now (see EnsureCombosRegistered).
	// CS keeps only its secondary in-menu overlay toggle.
	API::ComboId g_overlayOpenCombo = 0;
	API::ComboId g_overlayCloseCombo = 0;

	// CS's ButtonCombo (Utils/Input.h) and the helper's API::InputCombo are
	// layout-compatible but distinct types, so convert element-by-element. The
	// packed device/key values match, so a device enum round-trips through
	// uint32_t.
	std::vector<API::InputCombo> ToApi(const std::vector<ButtonCombo>& binds)
	{
		std::vector<API::InputCombo> out;
		out.reserve(binds.size());
		for (const auto& b : binds)
			out.emplace_back(static_cast<API::InputDeviceType>(static_cast<uint32_t>(b.GetDevice())), b.GetKey());
		return out;
	}

	// Build an on-rebind callback that writes the new chord back into the given
	// CS settings vector and persists settings to disk. `target` is a member of
	// the VR feature's settings singleton (stable address), captured by ref.
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
	// first Update() (settings are loaded by then). The main menu open/close are
	// owned by the helper — it focuses the last-opened overlay and documents the
	// binding in its welcome banner — so CS no longer registers those; it just
	// renders when the helper grants focus (RendersOnFocus).
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

namespace ImGuiVRHelperClient
{
	void Init()
	{
		// Init() runs at SKSE kPostPostLoad, after globals::ReInit() has cached
		// the VR flag, so prefer the global over REL::Module::IsVR().
		if (!globals::game::isVR)
			return;

		// kClientFlag_RendersOnFocus: we honor the helper's focus-render
		// contract — when the helper routes in-scene focus to us, we render the
		// menu into the panel RTV regardless of Menu::IsEnabled.
		if (!g_client.Connect("CommunityShaders", Plugin::VERSION.string().c_str(),
				API::kClientFlag_RendersOnFocus)) {
			logger::info("ImGuiVRHelper not detected; VR menus will only render on the desktop monitor");
			return;
		}

		logger::info("ImGuiVRHelper handshake successful (build {}), client_id={}",
			g_client.Helper()->GetBuildNumber(), g_client.Id());
	}

	bool IsRegistered() { return g_client.IsConnected(); }

	bool HelperRequestsRender() { return g_client.HasFocus(); }

	void RenderToPanel() { g_client.RenderToPanel(globals::d3d::context); }

	void FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
		float thumbstick_x, float thumbstick_y)
	{
		g_client.FeedVREvent(device, key_code, pressed, thumbstick_x, thumbstick_y);
	}

	void DrawBindingsTable() { g_client.DrawBindingsTable(); }

	void Update()
	{
		if (!g_client.IsConnected())
			return;

		EnsureCombosRegistered();

		auto* menu = globals::menu;
		if (!menu) {
			g_client.PumpInput(false);
			return;
		}

		// Reconcile the menu-open flag with helper focus (the single source of
		// truth for VR visibility): CS toggling its own menu drives focus; the
		// helper changing focus (open/close combo, cycle, swap) drives IsEnabled.
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

		g_client.PumpInput(focused, globals::features::vr.settings.mouseDeadzone);
	}
}
