#include "ImGuiVRHelperClient.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "Features/VR.h"  // VR::Settings + ButtonCombo (= InputCombo); pulls Menu.h
#include "Globals.h"
#include "ImGuiVRHelperAPI.h"

namespace
{
	namespace API = ImGuiVRHelperPluginAPI;

	API::IImGuiVRHelperInterface001* g_helper = nullptr;
	uint32_t g_clientId = 0;

	// Latched each helper Frame. OverlayRenderer reads this via
	// ImGuiVRHelperClient::HelperRequestsRender() to know whether to render the
	// menu independent of Menu::IsEnabled. Set when the helper's in-scene focus
	// model routes focus to us, so it expects fresh pixels in our panel RTV this
	// frame. Reading is racy (render thread vs OnFrame thread) but the value is
	// just a bool — torn reads don't matter.
	bool g_helperRequestsRender = false;

	constexpr uint32_t kFrameFlag_HasFocus = 1u << 0;

	// Controller snapshot latched from the helper's per-frame Frame (OnFrame
	// thread) and consumed on the render thread to drive ImGui IO. We latch the
	// level-state (held mask + sticks) rather than the helper's pre-computed
	// edges so the render-thread consumer can edge-detect against its own last
	// applied state — robust to any OnFrame/render rate mismatch.
	struct FrameSnapshot
	{
		uint32_t heldMask = 0;  // left.buttons_held | right.buttons_held (wire API::Button bits)
		float stickX = 0.0f;
		float stickY = 0.0f;
	};
	std::mutex g_frameMutex;
	FrameSnapshot g_frame;

	// Render-thread-only state.
	bool g_focusRequested = false;     // edge latch mirroring Menu::IsEnabled into helper focus
	bool g_hadFocus = false;           // latched once the helper grants us focus, for close-on-focus-loss
	bool g_combosRegistered = false;   // VR menu/overlay combos registered once with the helper
	uint32_t g_prevHeldMask = 0;       // previous consumed button mask, for click edge detection
	API::ComboId g_openCombo = 0;
	API::ComboId g_closeCombo = 0;
	API::ComboId g_overlayOpenCombo = 0;
	API::ComboId g_overlayCloseCombo = 0;

	void OnFrame(const API::Frame* f, void*)
	{
		g_helperRequestsRender = f && (f->flags & kFrameFlag_HasFocus) != 0;
		if (!f)
			return;

		// Pick the larger-magnitude stick axis across both hands so either
		// controller can scroll (matches SCS's old either-stick behavior).
		const float sx = (std::abs(f->left.stick_x) >= std::abs(f->right.stick_x)) ? f->left.stick_x : f->right.stick_x;
		const float sy = (std::abs(f->left.stick_y) >= std::abs(f->right.stick_y)) ? f->left.stick_y : f->right.stick_y;

		std::scoped_lock lk{ g_frameMutex };
		g_frame.heldMask = f->left.buttons_held | f->right.buttons_held;
		g_frame.stickX = sx;
		g_frame.stickY = sy;
	}

	// Translate an SCS keybind (vector<ButtonCombo>, packed device<<16|reKey,
	// identical layout to API::InputCombo) into a helper combo registration.
	// Returns 0 for an empty/unbound keybind.
	API::ComboId RegisterComboFromBinding(const std::vector<ButtonCombo>& binds)
	{
		if (binds.empty())
			return 0;
		std::vector<API::InputCombo> keys;
		keys.reserve(binds.size());
		for (const auto& b : binds) {
			keys.emplace_back(static_cast<API::InputDeviceType>(b.GetDevice()), b.GetKey());
		}
		// timeout_s is unused for simultaneous chords (the helper matches when
		// all keys are held at once); pass 0.
		return g_helper->RegisterCombo(g_clientId, keys.data(), keys.size(), 0.0f);
	}

	// Register the VR menu/overlay open/close combos with the helper once, on
	// the first Update() (settings are loaded by the time the menu first
	// renders). The helper has no UnregisterCombo, so live rebinding of these
	// VR keys won't take effect until the next launch — acceptable for v1.
	void EnsureCombosRegistered()
	{
		if (g_combosRegistered)
			return;
		const auto& s = globals::features::vr.settings;
		g_openCombo = RegisterComboFromBinding(s.VRMenuOpenKeys);
		g_closeCombo = RegisterComboFromBinding(s.VRMenuCloseKeys);
		g_overlayOpenCombo = RegisterComboFromBinding(s.VROverlayOpenKeys);
		g_overlayCloseCombo = RegisterComboFromBinding(s.VROverlayCloseKeys);
		g_combosRegistered = true;
		logger::info("ImGuiVRHelper: registered VR combos (menuOpen={}, menuClose={}, overlayOpen={}, overlayClose={})",
			g_openCombo, g_closeCombo, g_overlayOpenCombo, g_overlayCloseCombo);
	}

	// Keep helper focus in lockstep with whether our menu is open, edge-based so
	// we don't fight the helper's own settings UI for focus every frame. When
	// focused, the helper composites our panel RTV into the eyes.
	void SyncFocus(bool menuOpen)
	{
		if (menuOpen && !g_focusRequested) {
			g_helper->RequestFocus(g_clientId);
			g_focusRequested = true;
		} else if (!menuOpen && g_focusRequested) {
			g_helper->ReleaseFocus(g_clientId);
			g_focusRequested = false;
		}
	}

	// Feed the wand pointer + controller buttons into ImGui IO so the VR menu is
	// usable with the controller — the per-client OnFrame consumer the helper
	// delegates to clients (see imgui-vr-helper src/Input.cpp). Reimplements
	// SCS's old ProcessVRButtonEvent + ProcessControllerInputForImGui, sourced
	// from the helper's GetPointer + Frame instead of SCS's own controller state.
	// `menuOpen` gates the IO pokes (only when the menu is shown); the held mask
	// is tracked every frame so the first click after open isn't a stale edge.
	void ApplyVRInputToImGui(bool menuOpen)
	{
		uint32_t held = 0;
		float stickX = 0.0f, stickY = 0.0f;
		{
			std::scoped_lock lk{ g_frameMutex };
			held = g_frame.heldMask;
			stickX = g_frame.stickX;
			stickY = g_frame.stickY;
		}

		if (!menuOpen) {
			g_prevHeldMask = held;  // stay current so the next open edge-detects cleanly
			return;
		}

		ImGuiIO& io = ImGui::GetIO();

		// Cursor: wand-laser intersection on our panel, normalized [0..1] →
		// ImGui display coordinates (the panel renders at io.DisplaySize).
		//
		// WantSetMousePos is load-bearing: this consumer runs (from
		// ProcessInputEventQueue) BEFORE ImGui_ImplWin32_NewFrame and
		// Util::UpdateImGuiInput, both of which overwrite io.MousePos with the
		// desktop cursor (off-screen / -FLT_MAX while the mirror window is
		// unfocused, as it is in VR) every frame. Setting io.MousePos directly
		// and WantSetMousePos=true makes ImGui_ImplWin32 WARP the OS cursor to
		// our position, so UpdateImGuiInput's GetCursorPos reads it back
		// consistently. Without it the wand cursor was invisible/frozen until a
		// thumbstick nudge re-seeded the desktop cursor.
		// Only the intersection point matters here, not which controller produced it.
		float u = 0.0f, v = 0.0f;
		if (g_helper->GetPointer(g_clientId, &u, &v, nullptr)) {
			const float x = std::clamp(u * io.DisplaySize.x, 0.0f, io.DisplaySize.x);
			const float y = std::clamp(v * io.DisplaySize.y, 0.0f, io.DisplaySize.y);
			io.MousePos = ImVec2(x, y);
			io.AddMousePosEvent(x, y);
			io.MouseDrawCursor = true;
			io.WantSetMousePos = true;
		}

		// Buttons → mouse/keys, edge-detected against the previous held mask.
		const uint32_t changed = held ^ g_prevHeldMask;
		const auto onEdge = [&](API::Button b, auto&& fn) {
			const uint32_t bit = 1u << static_cast<uint32_t>(b);
			if (changed & bit)
				fn((held & bit) != 0);
		};
		onEdge(API::Button::TriggerClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, d); });
		onEdge(API::Button::GripClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Right, d); });
		onEdge(API::Button::PadClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
		onEdge(API::Button::StickClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
		onEdge(API::Button::BY, [&](bool d) { io.AddKeyEvent(ImGuiKey_Tab, d); });
		onEdge(API::Button::AX, [&](bool d) { io.AddKeyEvent(ImGuiKey_Enter, d); });
		g_prevHeldMask = held;

		// Thumbstick scroll (accumulated to discrete wheel ticks), matching
		// SCS's old ProcessThumbstickScroll feel.
		constexpr float kScrollAccumRate = 0.1f;      // stick deflection added to the accumulator per frame
		constexpr float kScrollTickThreshold = 0.3f;  // accumulator magnitude that emits one wheel tick
		const float deadzone = globals::features::vr.settings.mouseDeadzone;
		static float accumX = 0.0f, accumY = 0.0f;
		if (std::abs(stickX) > deadzone || std::abs(stickY) > deadzone) {
			accumX += stickX * kScrollAccumRate;
			accumY += stickY * kScrollAccumRate;
			float wheelX = 0.0f, wheelY = 0.0f;
			if (std::abs(accumX) > kScrollTickThreshold) {
				wheelX = accumX > 0.0f ? 1.0f : -1.0f;
				accumX = 0.0f;
			}
			if (std::abs(accumY) > kScrollTickThreshold) {
				wheelY = accumY > 0.0f ? 1.0f : -1.0f;
				accumY = 0.0f;
			}
			if (wheelX != 0.0f || wheelY != 0.0f)
				io.AddMouseWheelEvent(-wheelX, wheelY);
		}
	}
}

namespace ImGuiVRHelperClient
{
	void Init()
	{
		// Init() runs at SKSE kPostLoad, after globals::ReInit() has
		// cached the VR flag, so prefer the global over REL::Module::IsVR().
		if (!globals::game::isVR) {
			return;
		}

		g_helper = ImGuiVRHelperPluginAPI::GetImGuiVRHelperInterface001();
		if (!g_helper) {
			logger::info("ImGuiVRHelper not detected; VR menus will only render on the desktop monitor");
			return;
		}

		const auto version = Plugin::VERSION.string();

		// In-scene panel presentation (the free-floating quad in front of the
		// player) is implicit — every panel-mode client gets one.
		// kClientFlag_RendersOnFocus advertises that we honor the helper's
		// focus-render contract: when Frame.flags has client_has_focus,
		// OverlayRenderer renders the menu into the panel RTV regardless of
		// Menu::IsEnabled.
		const uint32_t flags = ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus;

		g_clientId = g_helper->RegisterClient(
			"CommunityShaders",
			version.c_str(),
			&OnFrame,
			nullptr,
			flags);

		if (g_clientId == 0) {
			logger::warn("ImGuiVRHelper RegisterClient failed; VR menus will only render on the desktop monitor");
			g_helper = nullptr;
			return;
		}

		logger::info("ImGuiVRHelper handshake successful (build {}), client_id={}, reported_version={}",
			g_helper->GetBuildNumber(), g_clientId, version);
	}

	bool IsRegistered()
	{
		return g_helper != nullptr && g_clientId != 0;
	}

	bool HelperRequestsRender()
	{
		return g_helperRequestsRender;
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

	void Update()
	{
		if (!IsRegistered()) {
			return;
		}

		EnsureCombosRegistered();

		// VR controller activation: poll the helper's combo state and flip the
		// menu/overlay open/close — restoring the controller activation that
		// used to live in VR::UpdateOverlayMenuStateFromInput (now gated off).
		if (auto* menu = globals::menu) {
			if (g_openCombo && g_helper->ComboFired(g_openCombo)) {
				menu->IsEnabled = true;
			}
			if (g_closeCombo && g_helper->ComboFired(g_closeCombo)) {
				menu->IsEnabled = false;
				// Also drop helper focus: when the menu was shown by a swap
				// (helper focus, IsEnabled already false), clearing IsEnabled
				// alone doesn't close it — the helper keeps compositing us until
				// focus is released.
				g_helper->ReleaseFocus(g_clientId);
				g_focusRequested = false;
				g_hadFocus = false;
			}
			if (menu->IsEnabled) {
				if (g_overlayOpenCombo && g_helper->ComboFired(g_overlayOpenCombo)) {
					menu->overlayVisible = true;
				}
				if (g_overlayCloseCombo && g_helper->ComboFired(g_overlayCloseCombo)) {
					menu->overlayVisible = false;
				}
			} else if (g_overlayOpenCombo || g_overlayCloseCombo) {
				// Drain overlay-combo latches while the menu is closed so they
				// don't fire stale on the next open.
				if (g_overlayOpenCombo)
					g_helper->ComboFired(g_overlayOpenCombo);
				if (g_overlayCloseCombo)
					g_helper->ComboFired(g_overlayCloseCombo);
			}
		}

		// Single-window swap: if our menu is open but the helper handed VR focus
		// to another overlay (another mod's hotkey, or the helper's own UI),
		// close our menu so the new overlay replaces it. g_hadFocus gates on
		// having actually held focus, so the one-frame grant lag on open doesn't
		// self-close us.
		if (g_helperRequestsRender) {
			g_hadFocus = true;
		} else if (g_hadFocus) {
			g_hadFocus = false;
			if (globals::menu && globals::menu->IsEnabled) {
				globals::menu->IsEnabled = false;
				logger::info("ImGuiVRHelper: lost VR focus to another overlay; closing CS menu");
			}
		}

		const bool menuOpen = globals::menu && globals::menu->IsEnabled;

		// Show our panel exactly while our menu is open.
		SyncFocus(menuOpen);

		// Drive cursor/clicks/scroll whenever our menu is SHOWN — our own open
		// OR the helper swapped focus to us (RendersOnFocus). Without the focus
		// case a swapped-to menu rendered but received no input.
		ApplyVRInputToImGui(menuOpen || HelperRequestsRender());
	}
}
