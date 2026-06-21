#pragma once

#include <cstdint>

// SCS-side adapter for the standalone ImGuiVRHelper SKSE plugin.
//
// Handles the SKSE-messaging handshake, registers SCS as TWO clients, and
// renders SCS ImGui draw data into the helper-owned panel RTVs from
// OverlayRenderer.
//
// Two surfaces (see Surface):
//   - Menu (kClientFlag_RendersOnFocus): the focus-driven, interactive
//     settings/editor panel. Carries all focus/combo/wand input — this is
//     the client every input/focus path below targets, and the one
//     IsRegistered() reports on. Stored as g_clientId.
//   - Hud (kClientFlag_HUDMode): an always-on, non-interactive overlay for
//     status that must be visible during gameplay/launch without the menu
//     being focused (shader-compilation progress, performance overlay,
//     shader-blocking status, feature-issue warnings, welcome banner).
//     No focus, no input, no on_frame. Stored as g_hudClientId.
//
// When the helper is registered, the SCS-internal VR overlay pipeline
// (SubmitOverlayFrame, ProcessVREvents, wand pointing, drag, combo
// recording, OpenVR overlay handles) is gated off; SCS instead forwards
// raw VR controller events into the helper via FeedVREvent so the helper
// drives wand pointing, combos, and overlay focus. The now-unused SCS
// overlay code is left behind the gate for now — a follow-up excises it
// once the migration is verified end-to-end.
//
// Helper-required policy: if the helper isn't installed, SCS does NOT
// fall back to its own VR overlay. VR users without the helper see menus
// only on the desktop monitor. (See vr-imgui-helper-plan in
// docs/development/.)

namespace ImGuiVRHelperClient
{
	/// Which helper-owned panel a render targets.
	enum class Surface
	{
		Menu,  ///< interactive settings/editor panel (g_clientId)
		Hud,   ///< always-on non-interactive overlay (g_hudClientId)
	};

	/// Performs the SKSE messaging handshake with ImGuiVRHelper and
	/// registers "CommunityShaders" (Menu surface) plus
	/// "CommunityShaders.HUD" (Hud surface) as clients. No-op if not
	/// running under VR or if the helper isn't installed. Safe to call
	/// once from kPostLoad. If the Menu client fails to register the
	/// adapter disables itself; if only the Hud client fails it logs a
	/// warning and continues (HUD overlays just won't show in VR).
	void Init();

	/// True if the handshake succeeded and we hold a valid MENU client_id.
	/// Use this to gate out SCS-internal VR overlay/input paths now
	/// owned by the helper. (The Hud client is best-effort and does not
	/// affect this.)
	bool IsRegistered();

	/// True iff the helper has requested SCS render its menu this
	/// frame (focus-render contract — kClientFlag_RendersOnFocus).
	/// Triggered when the helper's in-scene focus model routes focus
	/// to us. OverlayRenderer ORs this with Menu::IsEnabled to decide
	/// whether to draw the menu into the panel RTV.
	bool HelperRequestsRender();

	/// Renders the current ImGui draw data (ImGui::GetDrawData(), as
	/// produced by the most recent ImGui::Render()) into the specified
	/// surface's helper-owned panel RTV. Saves and restores the
	/// previously bound render target/viewport. No-op if the helper
	/// isn't registered, the targeted client id is 0, or that client's
	/// panel isn't yet available.
	///
	/// Must be called between ImGui::Render() and the next
	/// ImGui::NewFrame() so GetDrawData() returns valid data.
	void RenderToPanel(Surface surface);

	/// Per-frame VR input pump. Call once per rendered frame on the render
	/// thread BEFORE ImGui::NewFrame() (from Menu::ProcessInputEventQueue).
	/// Three jobs, all no-ops if the helper isn't registered:
	///   1. Polls the helper's combo state to open/close the menu (and the
	///      secondary overlay) — restores the VR controller activation that
	///      used to live in VR::UpdateOverlayMenuStateFromInput.
	///   2. Keeps helper focus in sync with Menu::IsEnabled so the helper
	///      composites our panel RTV into the eyes while the menu is open.
	///   3. Feeds the wand pointer (GetPointer) + controller buttons/stick
	///      into ImGui IO (cursor, click, scroll) so the VR menu is usable
	///      with the controller — the per-client OnFrame input consumer the
	///      helper delegates to clients.
	void Update();

	/// Forward a single VR controller event into the helper's input
	/// state. Translates SCS's RE::INPUT_DEVICE / scancode / thumbstick
	/// values to the helper's wire-stable wire format.
	///
	/// Parameters use raw RE::INPUT_DEVICE / RE::BSOpenVRControllerDevice::Keys
	/// values cast to uint32_t — kept untyped here to avoid pulling RE
	/// headers into this lightweight adapter.
	///
	/// Caller (Menu::ProcessInputEventQueue) is responsible for not
	/// invoking this when IsRegistered() is false.
	void FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
		float thumbstick_x, float thumbstick_y);
}
