// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// ImGuiVRHelper public API.
//
// Clients vendor api/ImGuiVRHelperAPI.h, api/ImGuiVRHelperAPI.cpp,
// api/ImGuiVRHelperTypes.h, and api/ImGuiVRHelperInput.h. After SKSE's
// kPostLoad fires, call GetImGuiVRHelperInterface001() to obtain a pointer
// to the helper's API. Subsequent calls go through the returned vtable —
// SKSE messaging is only used for the one-shot handshake.
//
// Handshake pattern derived from
//   https://github.com/alandtse/SkyrimVRESL/blob/master/src/SkyrimVRESLAPI.h
// originally from
//   https://github.com/adamhynek/higgs

#pragma once

#include <cstddef>
#include <cstdint>

#include <SKSE/SKSE.h>

#include "ImGuiVRHelperInput.h"
#include "ImGuiVRHelperTypes.h"

namespace ImGuiVRHelperPluginAPI
{
	/// SKSE plugin name (filename without extension) used for messaging dispatch.
	constexpr const auto kPluginName = "ImGuiVRHelper";

	/// Handshake message exchanged between client and helper at kPostLoad.
	struct Message
	{
		/// Randomly-generated, fixed for the lifetime of the helper. Clients
		/// dispatch this message; the helper's listener fills in
		/// `GetApiFunction` and returns.
		enum : uint32_t
		{
			kMessage_GetInterface = 0xEACB2BFAu  // randomly generated, fixed
		};

		/// Filled in by the helper. Returns the requested interface revision
		/// or nullptr if the helper doesn't support that revision.
		void* (*GetApiFunction)(uint32_t revisionNumber) = nullptr;
	};

	struct IImGuiVRHelperInterface001;

	/// One-shot handshake. Call once after kPostLoad. Returns nullptr if
	/// the helper isn't installed or is older than version 001.
	IImGuiVRHelperInterface001* GetImGuiVRHelperInterface001();

	/// Versioned interface. Future revisions extend by inheritance:
	///   struct IImGuiVRHelperInterface002 : IImGuiVRHelperInterface001 { ... };
	/// The helper's GetApiFunction returns the highest revision the
	/// installed build supports, or nullptr for revisions newer than itself.
	struct IImGuiVRHelperInterface001
	{
		/// Helper build number; informational. Clients should not key
		/// behavior off this — use interface revision instead.
		virtual uint32_t GetBuildNumber() = 0;

		// ---- Lifecycle --------------------------------------------------

		/// Register a client. Returns a non-zero client_id on success, 0 on
		/// failure (e.g. helper not yet initialized, or name already taken).
		/// `name` and `version` are copied internally; pass `nullptr` for
		/// `version` to omit it. `flags` is a bitmask of ClientFlags.
		///
		/// Recommended `version` content: the calling mod's human-readable
		/// version (e.g. SKSE PluginVersionData::string()). The helper does
		/// not parse it — surfaced verbatim in the helper's "Registered
		/// Clients" diagnostic table.
		virtual uint32_t RegisterClient(const char* name, const char* version,
			OnFrameFn on_frame, void* user, uint32_t flags) = 0;

		/// Unregister a previously-registered client. Releases the panel
		/// render target, cancels any combo recording, and stops invoking
		/// `on_frame`. Safe to call from any SKSE message stage.
		virtual void UnregisterClient(uint32_t client_id) = 0;

		// ---- Texture handoff -------------------------------------------

		/// Get the helper-owned render target for this client's panel.
		/// Returns false if the client hasn't been issued one yet (e.g.
		/// before first frame). The returned `rtv` is valid until
		/// UnregisterClient. `width` and `height` may change between calls.
		virtual bool GetPanel(uint32_t client_id, PanelHandle* out) = 0;

		// ---- Pointer / cursor ------------------------------------------

		/// If a wand laser is currently intersecting this client's panel,
		/// fills `*u`/`*v` with normalized panel coordinates [0..1] and
		/// `*device_idx` with the source controller's tracked device index,
		/// then returns true. Otherwise returns false.
		virtual bool GetPointer(uint32_t client_id, float* u, float* v,
			uint32_t* device_idx) = 0;

		// ---- Combos -----------------------------------------------------

		/// Register a button combo for this client. Returns a ComboId the
		/// client polls each frame via ComboFired. The helper handles all
		/// matching, sequence timing, and one-shot edge detection.
		///
		/// `label` is a short human-readable name for the combo ("Open menu"),
		/// shown in the helper's controller-mapping UI so users can tell a
		/// client's combos apart. May be null/empty.
		///
		/// `on_rebind` (may be null) is invoked when the user rebinds this combo
		/// from the controller map: the helper updates the live keys AND calls
		/// back with the new chord so the client can persist it. `user` is passed
		/// through to that callback.
		virtual ComboId RegisterCombo(uint32_t client_id, const InputCombo* keys,
			std::size_t n, float timeout_s, const char* label,
			ComboRebindFn on_rebind, void* user) = 0;

		/// Edge-triggered: returns true exactly once per combo activation
		/// and resets internal latch.
		virtual bool ComboFired(ComboId) = 0;

		/// Open the helper's modal "press buttons now" capture overlay.
		/// While capture is active, the requesting client implicitly holds
		/// modal focus regardless of prior state. On completion or timeout,
		/// `on_done` is invoked and prior focus is restored.
		virtual void StartComboRecording(uint32_t client_id, const char* label,
			ComboRecordedFn on_done, void* user, float timeout_s) = 0;

		/// Cancels an in-progress recording for this client (if any).
		virtual void CancelComboRecording(uint32_t client_id) = 0;

		// ---- Focus / visibility ----------------------------------------

		/// True iff the user has the VR overlay open (any client visible).
		virtual bool IsOverlayVisible() = 0;

		/// Request modal focus for this client. Used when the client opens
		/// its menu in response to a hotkey. Subsequent input is delivered
		/// to this client and synthesized cursor input is suppressed.
		virtual void RequestFocus(uint32_t client_id) = 0;

		/// Release focus. The helper picks the next focused client based on
		/// LRU.
		virtual void ReleaseFocus(uint32_t client_id) = 0;

		// ---- Haptics ----------------------------------------------------

		/// Trigger a haptic pulse on the controller identified by
		/// `haptic_token` (taken from a Frame's Hand::haptic_token).
		virtual void TriggerHaptic(uint32_t client_id, uint32_t haptic_token,
			uint32_t duration_us, float frequency, float amplitude) = 0;

		// ---- Migration helper ------------------------------------------

		/// Import settings from a legacy client that was relinquishing
		/// control of overlay-side configuration to the helper. The helper
		/// merges these into its own JSON store. One-shot; subsequent
		/// imports are ignored.
		virtual bool ImportLegacySettings(const char* json_blob) = 0;

		// ---- Input ingestion -------------------------------------------

		/// Push one VR controller event into the helper's input state.
		/// Clients that own a PollInputDevices-style hook (e.g. SCS) call
		/// this once per VR event to keep the helper's controller state
		/// fresh; the helper then drives wand pointing, drag, and combo
		/// matching off it.
		///
		/// Parameters:
		///   - device:         RE::INPUT_DEVICE value (kVivePrimary,
		///                     kOculusSecondary, etc.); cast-compatible
		///                     with uint32_t for ABI stability.
		///   - key_code:       RE::BSOpenVRControllerDevice::Keys value
		///                     (kBY=1, kGrip=2, kXA=7, ...).
		///   - pressed:        true on press, false on release.
		///   - thumbstick_x/y: raw analog values [-1..1] for thumbstick
		///                     events; pass 0 for button events.
		///
		/// A future revision will install the PollInputDevices hook
		/// directly so clients can drop their own input plumbing.
		virtual void FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
			float thumbstick_x, float thumbstick_y) = 0;

		// ---- SteamVR Dashboard ------------------------------------------

		/// True iff the SteamVR dashboard is currently open. Useful for
		/// clients that want to suppress in-scene rendering while their
		/// panel is the active dashboard surface (avoids double-paint
		/// when the user is interacting with the dashboard).
		///
		/// The helper owns a single shared dashboard surface; a picker
		/// inside the helper's settings panel chooses which
		/// kClientFlag_Dashboard client's panel texture is mirrored.
		/// Per-client thumbnails / individual rail entries are not part
		/// of the v1 design — keeps the SteamVR rail uncluttered as the
		/// helper picks up more clients.
		virtual bool IsDashboardVisible() = 0;

		// ---- Combo rebinding -------------------------------------------

		/// Rebind a previously-registered combo to a new chord (e.g. driven by an
		/// in-app bindings table). Updates the live keys the helper matches AND
		/// invokes the combo's `on_rebind` callback so the owner can persist it.
		/// No-op if `combo` is unknown or `n` is 0 / out of range.
		virtual void RebindCombo(ComboId combo, const InputCombo* keys,
			std::size_t n) = 0;
	};

}  // namespace ImGuiVRHelperPluginAPI
