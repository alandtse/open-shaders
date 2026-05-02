#pragma once

#include <cstdint>

// SCS-side adapter for the standalone ImGuiVRHelper SKSE plugin.
//
// Phases 1-2: handshake, client registration, render the SCS ImGui
// draw data into the helper-owned panel RTV from
// OverlayRenderer::FinalizeImGuiFrame.
//
// Phases 3-4: when the helper is registered, the SCS-internal VR
// overlay pipeline (SubmitOverlayFrame, ProcessVREvents, wand
// pointing, drag, combo recording, OpenVR overlay handles) is
// gated off. SCS forwards raw VR controller events into the helper
// via FeedVREvent so the helper drives wand pointing, combos, and
// overlay focus. The dead SCS code is left in place behind the
// gate for now — a follow-up will excise it once the migration is
// verified end-to-end.
//
// Helper-required policy: if the helper isn't installed, SCS does
// NOT fall back to its own VR overlay. VR users without the helper
// see menus only on the desktop monitor. (See vr-imgui-helper-plan
// in docs/development/.)

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

	/// Renders the current ImGui draw data (as produced by the most
	/// recent ImGui::Render() in OverlayRenderer::FinalizeImGuiFrame)
	/// into the helper's panel RTV. Saves and restores the previously
	/// bound render target. No-op if the helper isn't registered or
	/// the panel isn't yet available.
	///
	/// Must be called between ImGui::Render() and the next
	/// ImGui::NewFrame() so GetDrawData() returns valid data.
	void RenderToPanel();

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
