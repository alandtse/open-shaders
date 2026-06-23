#pragma once

// Always-on VR status HUD.
//
// In VR, CS renders its whole ImGui frame into one focus-driven ImGuiVRHelper
// panel, so always-on status overlays (shader-compilation progress, etc.) are
// invisible unless the menu is focused. This renders them into a dedicated,
// non-interactive ImGui context blitted to a HUD-mode helper client, which the
// helper composites over the scene every frame.
//
// A SEPARATE context is load-bearing: a previous attempt rendered these as a
// second pass on the menu's own ImGui context, which cleared the menu's input
// state every frame and broke all interaction. Each ImGui context supports only
// one frame per render, so the HUD gets its own.

namespace StatusHUD
{
	// Render the VR status HUD for this frame. No-op on desktop or when the
	// helper isn't registered. Call once per frame after the main overlay.
	void Render();

	// True when the VR status HUD owns the always-on status overlays this frame,
	// so the main (menu) overlay skips them to avoid a double-render.
	bool OwnsStatusOverlays();

	// Destroy the dedicated context + DX11 backend. Safe if never initialized.
	void Shutdown();
}
