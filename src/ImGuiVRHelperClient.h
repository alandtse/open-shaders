#pragma once

// SCS-side adapter for the standalone ImGuiVRHelper SKSE plugin.
//
// Phase 2: SCS still drives ImGui itself — NewFrame, draw calls, and a
// final ImGui_ImplDX11_RenderDrawData() targeting the desktop swapchain
// all live in OverlayRenderer. After that desktop pass, we additionally
// re-render the same ImDrawData into the helper-owned panel RTV so it
// can composite our menu as a 3D quad in the HMD. This is purely
// additive — SCS's own VR::SubmitOverlayFrame still runs as a fallback
// for VR users without the helper installed.
//
// Phases 3-4 will retire the SCS-internal VR overlay path entirely once
// the helper-rendered panel is verified.

namespace ImGuiVRHelperClient
{
	/// Performs the SKSE messaging handshake with ImGuiVRHelper and
	/// registers "CommunityShaders" as a client. No-op if not running
	/// under VR or if the helper isn't installed. Safe to call once
	/// from kPostLoad.
	void Init();

	/// True if the handshake succeeded and we hold a valid client_id.
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
}
