// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// Wire-stable data structures for the ImGuiVRHelper public API.
//
// IMPORTANT: this header must NOT include ImGui or OpenVR headers, must NOT
// reference any ImGuiKey/IVR* type, and must NOT change layout once
// shipped. Add new fields by bumping kInputAbiVersion and extending the
// trailing portion of structs, never by reordering existing fields.

#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11RenderTargetView;

namespace ImGuiVRHelperPluginAPI
{
	inline constexpr uint32_t kInputAbiVersion = 1;

	/// 3D pose in OpenVR standing-universe space, meters and quaternions.
	struct Pose
	{
		float pos[3];     ///< meters; x right, y up, z toward user
		float orient[4];  ///< quaternion w, x, y, z
		float vel[3];
		float angvel[3];
		uint32_t valid;  ///< bit0 tracked, bit1 visible, bit2 has_velocity
	};

	/// Wire-stable button identifiers. Owned by the helper; never renumbered.
	/// The helper translates from RE::BSOpenVRControllerDevice::Keys at the
	/// boundary so this enum is independent of the OpenVR/SkyrimVR mapping.
	enum class Button : uint32_t
	{
		AX = 0,            ///< RE Keys::kXA              (7)  — X (left) / A (right) face button
		BY = 1,            ///< RE Keys::kBY              (1)  — Y (left) / B (right) face button
		Menu = 2,          ///< system menu / app menu   (varies by controller)
		System = 3,        ///< system / home button     (varies)
		TriggerClick = 4,  ///< RE Keys::kTrigger         (33) — analog trigger pulled past click threshold
		GripClick = 5,     ///< RE Keys::kGrip            (2)  — grip pressed; kGripAlt (34) folded in
		StickClick = 6,    ///< RE Keys::kJoystickTrigger (32) — thumbstick pressed in
		PadClick = 7,      ///< RE Keys::kTouchpadClick   (35) — touchpad clicked; kTouchpadAlt (36) folded in
		Shoulder = 8,      ///< reserved for future controllers with shoulder buttons
		Reserved9 = 9,
		Reserved10 = 10,
		Reserved11 = 11,
		Reserved12 = 12,
		Reserved13 = 13,
		Reserved14 = 14,
		Reserved15 = 15,
	};

	/// Per-controller per-frame state.
	struct Hand
	{
		uint32_t connected;
		uint32_t controller_kind;  ///< 0 unknown, 1 index, 2 oculus_touch, 3 wmr, 4 vive, 5 cosmos, ...
		Pose pose;
		uint32_t buttons_held;      ///< bitmask: (1u << static_cast<uint32_t>(Button::X))
		uint32_t buttons_pressed;   ///< edges this frame
		uint32_t buttons_released;  ///< edges this frame
		uint32_t buttons_touched;   ///< capacitive
		float trigger;              ///< 0..1 analog
		float grip;                 ///< 0..1 analog
		float stick_x, stick_y;     ///< -1..1
		float pad_x, pad_y;         ///< -1..1
		uint32_t haptic_token;      ///< opaque; pass back to TriggerHaptic()
	};

	/// Per-frame snapshot delivered to each registered client via OnFrameFn.
	struct Frame
	{
		uint32_t abi_version;  ///< == kInputAbiVersion
		uint32_t struct_size;  ///< == sizeof(Frame); allows forward-extension
		float dt;              ///< seconds since previous frame
		Pose hmd;
		Hand left;
		Hand right;
		uint32_t flags;  ///< bit0 client_has_focus
						 ///< bit1 overlay_visible
						 ///< bit2 client_pointer_in_panel
	};

	/// Helper-owned render target the client renders its ImGui frame into.
	/// Valid until UnregisterClient. Width/height may change between frames
	/// if the user resizes the panel; clients should re-check each frame.
	struct PanelHandle
	{
		uint32_t width;
		uint32_t height;
		ID3D11RenderTargetView* rtv;
	};

	using ComboId = uint32_t;

	/// Forward declaration; defined in ImGuiVRHelperInput.h.
	struct InputCombo;

	using OnFrameFn = void (*)(const Frame*, void* user);
	using ComboRecordedFn = void (*)(const InputCombo*, std::size_t n, void* user);

	/// Bit flags for RegisterClient().
	enum ClientFlags : uint32_t
	{
		kClientFlag_None = 0,
		kClientFlag_RequiresFocus = 1u << 0,  ///< only invoke on_frame when this client holds focus

		/// Render this client's panel as a fully transparent, always-on
		/// overlay anchored to the HMD at a comfortable viewing depth —
		/// like Skyrim's vanilla HUD plane. The client doesn't have to
		/// reason about positioning; the helper picks a depth + size
		/// that fills most of the user's FOV.
		///
		/// Use cases: subtitle text, damage numbers, nameplates,
		/// always-on debug overlays — any flat 2D content you'd put on
		/// a HUD layer in flat-screen Skyrim.
		///
		/// Behavior:
		///   - The panel RTV is the same 1920×1080 RGBA8 the helper
		///     hands every client. Clear it transparent (ImGui windows
		///     with NoBackground / alpha 0) and draw text / shapes at
		///     the panel coordinates you want.
		///   - The helper renders the RTV as a 3D quad at fixed
		///     HMD-relative depth (~1.5m forward, ~2.5m wide), with
		///     proper per-eye projection so the eyes converge on it.
		///     Transparent pixels pass through to the underlying scene.
		///   - HUD clients are NOT subject to focus or attachMode
		///     gating — they render every frame they exist. The
		///     focused panel-mode client (if any) renders separately
		///     at its own (movable, draggable) position.
		///   - Multiple HUD clients stack in registration order;
		///     last-registered draws on top.
		kClientFlag_HUDMode = 1u << 1,

		/// Also surface this client as a SteamVR Dashboard overlay. The
		/// client appears as an icon in the SteamVR dashboard's left rail;
		/// clicking it pops out the client's panel as a 3D plane the user
		/// can pin / move / resize through SteamVR's standard dashboard
		/// gestures.
		///
		/// Orthogonal to kClientFlag_HUDMode and to the in-scene panel —
		/// a dashboard client may also be an in-scene panel client (most
		/// common case: same panel, two surfaces). HUD-mode + Dashboard
		/// is allowed but unusual.
		///
		/// Behaviour:
		///   - The panel RTV is reused; SteamVR makes its own copy on
		///     SetOverlayTextureFromHandle, so one render produces both
		///     the in-scene quad and the dashboard plane.
		///   - SteamVR drives input via VREvent_Mouse* delivered to the
		///     overlay; the helper translates those to ImGui mouse state
		///     for the focused client. No wand pointing involved on this
		///     path — the dashboard's own laser handles it.
		///   - The dashboard plane renders only while the SteamVR
		///     dashboard is open; the in-scene panel takes over when
		///     it's closed.
		///   - Set the thumbnail asset via SetDashboardThumbnail (PNG
		///     path) after registration, or pass nullptr and accept the
		///     default helper icon.
		///
		/// Compatibility note: dashboard overlays require the SteamVR
		/// IVROverlay implementation. OpenComposite-based runtimes
		/// implement this only partially; the helper detects this at
		/// CreateDashboardOverlay time and gracefully degrades to the
		/// in-scene-only path (logged once at registration).
		kClientFlag_Dashboard = 1u << 2,

		/// Acknowledge the focus-render contract: when this client's
		/// per-frame Frame.flags has bit0 (client_has_focus) set, the
		/// client WILL render its menu UI into the panel RTV that
		/// frame, regardless of whatever internal "is my menu open"
		/// state the client tracks.
		///
		/// This is the wire signal the helper uses to tell a client
		/// "you're being shown right now — please draw something."
		/// Triggers include:
		///   - User selected this client in the helper's dashboard
		///     picker (SteamVR rail interaction).
		///   - The helper's in-scene focus model picked this client
		///     (e.g. user just dismissed another panel).
		///
		/// Without this flag, the helper still tracks focus on the
		/// client (so RequestFocus / ReleaseFocus still work), but
		/// assumes the client renders only when its own internal
		/// trigger fires (e.g. a TAB hotkey). The dashboard picker
		/// for such clients shows a "trigger this manually" banner
		/// instead of trying to mirror a possibly-stale panel RTV.
		///
		/// New code SHOULD set this flag. Pre-existing clients that
		/// can't easily move their render to focus-driven (legacy
		/// menu state machines, etc.) can omit it and the helper
		/// degrades gracefully.
		kClientFlag_RendersOnFocus = 1u << 3,
	};

}  // namespace ImGuiVRHelperPluginAPI
