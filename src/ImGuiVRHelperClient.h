#pragma once

#include <cstdint>

// SCS-side adapter for the standalone ImGuiVRHelper SKSE plugin.
//
// Handles the SKSE-messaging handshake, registers SCS as a client, and
// renders the SCS ImGui draw data into the helper-owned panel RTV from
// OverlayRenderer::FinalizeImGuiFrame.
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
	/// Performs the SKSE messaging handshake with ImGuiVRHelper and
	/// registers "CommunityShaders" as a client. No-op if not running
	/// under VR or if the helper isn't installed. Safe to call once
	/// from kPostLoad.
	void Init();

	/// True if the handshake succeeded and we hold a valid client_id.
	/// Use this to gate out SCS-internal VR overlay/input paths now
	/// owned by the helper.
	bool IsRegistered();

	/// True iff the helper has requested SCS render its menu this
	/// frame (focus-render contract — kClientFlag_RendersOnFocus).
	/// Triggered when the helper's in-scene focus model routes focus
	/// to us. OverlayRenderer ORs this with Menu::IsEnabled to decide
	/// whether to draw the menu into the panel RTV.
	bool HelperRequestsRender();

	/// Renders the current ImGui draw data (as produced by the most
	/// recent ImGui::Render() in OverlayRenderer::FinalizeImGuiFrame)
	/// into the helper's panel RTV. Saves and restores the previously
	/// bound render target. No-op if the helper isn't registered or
	/// the panel isn't yet available.
	///
	/// Must be called between ImGui::Render() and the next
	/// ImGui::NewFrame() so GetDrawData() returns valid data.
	void RenderToPanel();

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

	/// Draw a sortable / filterable table of the VR menu/overlay bindings,
	/// color-coded per controller, with a per-row Rebind button (live capture
	/// through the helper, persisted to CS settings). Restores the bindings
	/// review/rebind UI that VR lost. Call from inside an ImGui window in the
	/// VR settings page. No-op if the helper isn't registered.
	void DrawBindingsTable();
}
