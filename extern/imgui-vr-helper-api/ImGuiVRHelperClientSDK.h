// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// ImGuiVRHelper client SDK — a header-only convenience layer over the raw
// IImGuiVRHelperInterface001 vtable. It absorbs the boilerplate every ImGui
// mod needs to gain VR support: the handshake, the per-frame controller
// snapshot, focus sync, the wand-cursor / button / thumbstick input pump, the
// panel-RTV blit, and combo registration/rebinding. Adopting VR for an
// existing ImGui menu then looks like:
//
//   ImGuiVRHelperPluginAPI::Client g_vr;
//   // at SKSE kPostPostLoad (fires after every plugin's kPostLoad, so the
//   // helper's listener is registered regardless of load order):
//   g_vr.Connect("MyMod", versionStr, ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus);
//   g_vr.AddCombo("Open menu", myOpenKeys, [](auto* k, auto n){ /* persist */ });
//   // each render frame:
//   if (g_vr.Fired(openCombo)) menuOpen = true;
//   g_vr.ReconcileFocus(menuOpen);  // syncs menuOpen <-> helper focus both ways
//   g_vr.PumpInput(menuOpen);
//   // build your ImGui frame + ImGui::Render(), then:
//   g_vr.RenderToPanel(myD3DContext);
//
// For an always-on HUD layer, the whole context lifecycle is one call:
//   g_vr.RenderHud(device, ctx, displaySize, []{ /* your ImGui draws */ });
//
// Plus a drop-in, sortable/filterable bindings table for your own settings UI:
//
//   g_vr.DrawBindingsTable();
//
// Hard dependencies (include from a TU that has them): Dear ImGui (<imgui.h> +
// <imgui_internal.h>) and the official DX11 backend (<imgui_impl_dx11.h>),
// <d3d11.h>, and nlohmann/json (pulled by ImGuiVRHelperInput.h). RenderHud /
// RenderToPanel call ImGui_ImplDX11_* directly, so a mod shipping a custom
// (non-stock) ImGui backend can't use those methods. It does NOT depend on
// CommonLibSSE/RE — button names use OpenVR key codes — so it is safe to
// vendor alongside the other api/ headers.

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_internal.h>  // RenderHud re-allows window SetWindowPos*AllowFlags

#include "ImGuiVRHelperAPI.h"
#include "ImGuiVRHelperInput.h"
#include "ImGuiVRHelperTypes.h"

namespace ImGuiVRHelperPluginAPI
{
	/// Per-controller color encoding, matching the helper's controller-map UI:
	/// Primary = yellow, Secondary = blue, Both = green, anything else = white.
	inline ImVec4 DeviceColor(InputDeviceType device)
	{
		switch (device) {
		case InputDeviceType::Primary:
			return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // yellow
		case InputDeviceType::Secondary:
			return ImVec4(0.0f, 0.5f, 1.0f, 1.0f);  // blue
		case InputDeviceType::Both:
			return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // green
		default:
			return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // white
		}
	}

	/// Human-readable name for a packed key code. VR codes are
	/// RE::BSOpenVRControllerDevice::Keys values; keyboard/mouse/gamepad
	/// devices fall through to a generic "Key N" so the table never shows a
	/// bare number with no label.
	inline std::string ButtonName(InputDeviceType device, uint32_t key)
	{
		if (device == InputDeviceType::Primary || device == InputDeviceType::Secondary ||
			device == InputDeviceType::Both) {
			switch (key) {
			case 1:
				return "B/Y";
			case 2:
				return "Grip";
			case 7:
				return "A/X";
			case 32:
				return "Stick Click";
			case 33:
				return "Trigger";
			case 34:
				return "Grip";  // kGripAlt — same physical button as kGrip
			case 35:
				return "Touchpad";
			case 36:
				return "Touchpad Alt";
			default:
				break;
			}
		}
		return "Key " + std::to_string(key);
	}

	/// RAII guard: captures the current ImGui context and restores it on scope
	/// exit. Use around any code that switches to a private context — leaving a
	/// foreign context current crashes the host's ImGui_ImplWin32_NewFrame.
	struct ScopedImGuiContext
	{
		ImGuiContext* prev = ImGui::GetCurrentContext();
		~ScopedImGuiContext() { ImGui::SetCurrentContext(prev); }
		ScopedImGuiContext() = default;
		ScopedImGuiContext(const ScopedImGuiContext&) = delete;
		ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
	};

	/// True if the helper plugin is installed and its interface is available,
	/// without registering. Lets a mod decide up front whether to adopt the VR
	/// path or fall back to desktop-only, and tell "not installed" from a failed
	/// Connect() (a false Connect after this returned true means registration was
	/// rejected, e.g. a duplicate client name).
	[[nodiscard]] inline bool IsHelperInstalled() { return GetImGuiVRHelperInterface001() != nullptr; }

	/// Convenience wrapper around the helper interface. One instance per mod;
	/// not thread-safe except for the OnFrame snapshot, which is mutex-guarded
	/// so the helper's frame thread and your render thread can share it.
	///
	/// ImGui ownership: YOU bring your own statically-linked ImGui + DX11 backend.
	/// The helper only owns the panel RTV and the controller input source; it never
	/// shares a GImGui with you. PumpInput/RenderToPanel act on your current
	/// context; RenderHud owns a private context of its own. Call PumpInput and
	/// RenderToPanel on your render thread (after your NewFrame / Render
	/// respectively). The OnFrame callback runs on the helper's frame thread — the
	/// SDK already mutex-guards the bit of state it shares with your render thread.
	class Client
	{
	public:
		/// Called with the new chord when a registered combo is rebound (from
		/// the helper's controller map or this SDK's bindings table), so the
		/// owner can persist it. The live keys are already updated.
		using RebindCallback = std::function<void(const InputCombo* keys, std::size_t n)>;

		Client() = default;
		Client(const Client&) = delete;
		Client& operator=(const Client&) = delete;

		// ---- Lifecycle -------------------------------------------------

		/// Perform the handshake and register as a client. Call at kPostPostLoad
		/// (so the helper's listener is up regardless of load order); retryable if
		/// you call earlier. Returns true once the helper is found and registration
		/// succeeds. `flags` is a bitmask of kClientFlag_*.
		bool Connect(const char* name, const char* version, uint32_t flags)
		{
			if (m_id != 0)
				return true;
			m_helper = GetImGuiVRHelperInterface001();
			if (!m_helper)
				return false;
			m_id = m_helper->RegisterClient(name, version, &OnFrameThunk, this, flags);
			if (m_id == 0) {
				m_helper = nullptr;
				return false;
			}
			return true;
		}

		void Disconnect()
		{
			if (m_helper && m_id)
				m_helper->UnregisterClient(m_id);
			m_helper = nullptr;
			m_id = 0;
		}

		[[nodiscard]] bool IsConnected() const { return m_helper != nullptr && m_id != 0; }
		[[nodiscard]] uint32_t Id() const { return m_id; }
		[[nodiscard]] IImGuiVRHelperInterface001* Helper() const { return m_helper; }

		/// True when the helper routed in-scene focus to this client this frame
		/// (it expects fresh pixels in the panel RTV even if your own menu flag
		/// is closed — e.g. the user cycled overlays to you).
		[[nodiscard]] bool HasFocus() const { return m_requestsRender; }

		// ---- Combos ----------------------------------------------------

		/// Register a chord. `onRebind` (optional) fires with the new chord
		/// whenever the user rebinds, clears, or resets it, so you can persist
		/// your bindings (an empty chord means unbound). `defaultKeys` (optional)
		/// is the factory default for this binding — pass it to enable the
		/// per-row "Reset" button in DrawBindingsTable. Returns 0 for an empty
		/// initial chord or if not connected.
		ComboId AddCombo(const char* label, const std::vector<InputCombo>& keys,
			RebindCallback onRebind = {}, const std::vector<InputCombo>& defaultKeys = {})
		{
			if (!IsConnected() || keys.empty())
				return 0;
			ComboEntry entry;
			entry.owner = this;
			entry.label = label ? label : "";
			entry.keys = keys;
			entry.onRebind = std::move(onRebind);
			entry.defaults = defaultKeys;
			m_combos.push_back(std::move(entry));
			ComboEntry& e = m_combos.back();
			e.id = m_helper->RegisterCombo(m_id, e.keys.data(), e.keys.size(), 0.0f,
				e.label.c_str(), &RebindThunk, &e);
			return e.id;
		}

		/// Edge-triggered: true exactly once per activation.
		bool Fired(ComboId id) { return IsConnected() && id != 0 && m_helper->ComboFired(id); }

		// ---- Per-frame plumbing ----------------------------------------

		/// Keep helper focus in lockstep with your menu-open flag (edge-based,
		/// so it doesn't fight the helper's own UI for focus every frame).
		/// Deprecated: ReconcileFocus does this plus the helper->client direction;
		/// don't mix the two (they track separate latches).
		[[deprecated("use ReconcileFocus")]] void SyncFocus(bool menuOpen)
		{
			if (!IsConnected())
				return;
			if (menuOpen && !m_focusRequested) {
				m_helper->RequestFocus(m_id);
				m_focusRequested = true;
			} else if (!menuOpen && m_focusRequested) {
				m_helper->ReleaseFocus(m_id);
				m_focusRequested = false;
			}
		}

		/// Unconditionally request helper focus (show this client's overlay). Use
		/// when the client opens its menu itself (e.g. a keyboard shortcut) so the
		/// helper composites it. Pair with ReleaseFocus() to close.
		void RequestFocus()
		{
			if (!IsConnected())
				return;
			m_helper->RequestFocus(m_id);
			m_focusRequested = true;
		}

		/// Unconditionally release helper focus and reset focus latches. Use
		/// when closing in response to a hotkey: covers the swap case where the
		/// helper granted focus without an explicit RequestFocus (so SyncFocus
		/// alone wouldn't release it).
		void ReleaseFocus()
		{
			if (!IsConnected())
				return;
			m_helper->ReleaseFocus(m_id);
			m_focusRequested = false;
			m_hadFocus = false;
		}

		/// Returns true exactly once when the helper took focus away while you
		/// held it (the user opened another overlay) — close your menu so the
		/// new overlay replaces it (single-window swap).
		/// Deprecated: ReconcileFocus folds this in; don't mix the two.
		[[deprecated("use ReconcileFocus")]] bool ConsumeFocusLost()
		{
			if (m_requestsRender) {
				m_hadFocus = true;
				return false;
			}
			if (m_hadFocus) {
				m_hadFocus = false;
				return true;
			}
			return false;
		}

		/// One-call focus reconciliation — replaces SyncFocus + ConsumeFocusLost +
		/// the manual edge logic. Pass your menu-open flag by reference: when you
		/// toggle it, helper focus follows; when the helper changes focus (the user
		/// cycled overlays to/from you), your flag follows. Returns the resolved
		/// shown state — feed it to PumpInput and use it to gate NewFrame/Render.
		bool ReconcileFocus(bool& menuOpen)
		{
			if (!IsConnected())
				return menuOpen;
			const bool focused = HasFocus();
			if (menuOpen != m_prevMenuOpen) {
				if (menuOpen)
					RequestFocus();
				else
					ReleaseFocus();
			} else if (focused != menuOpen) {
				menuOpen = focused;  // helper changed focus (overlay swap) — mirror it
			}
			m_prevMenuOpen = menuOpen;
			return menuOpen;
		}

		/// Pump the wand cursor, controller buttons, and thumbstick scroll into
		/// the current ImGui context's IO. Call with `active` true while your
		/// menu is shown (your own open OR HasFocus()); pass it false otherwise
		/// so button edges stay current without poking IO.
		void PumpInput(bool active, float scrollDeadzone = 0.15f)
		{
			if (!IsConnected())
				return;

			uint32_t held = 0;
			float stickX = 0.0f, stickY = 0.0f;
			{
				std::scoped_lock lk{ m_mutex };
				held = m_heldMask;
				stickX = m_stickX;
				stickY = m_stickY;
			}

			if (!active) {
				m_prevHeld = held;  // stay current so the next open edge-detects cleanly
				return;
			}

			ImGuiIO& io = ImGui::GetIO();

			// Wand-laser intersection → ImGui cursor. WantSetMousePos warps the
			// OS cursor so the platform backend reads our position back instead
			// of the (off-screen, in VR) desktop cursor.
			float u = 0.0f, v = 0.0f;
			if (m_helper->GetPointer(m_id, &u, &v, nullptr)) {
				const float x = std::clamp(u * io.DisplaySize.x, 0.0f, io.DisplaySize.x);
				const float y = std::clamp(v * io.DisplaySize.y, 0.0f, io.DisplaySize.y);
				io.MousePos = ImVec2(x, y);
				io.AddMousePosEvent(x, y);
				io.MouseDrawCursor = true;
				io.WantSetMousePos = true;
			}

			const uint32_t changed = held ^ m_prevHeld;
			const auto onEdge = [&](Button b, auto&& fn) {
				const uint32_t bit = 1u << static_cast<uint32_t>(b);
				if (changed & bit)
					fn((held & bit) != 0);
			};
			onEdge(Button::TriggerClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, d); });
			onEdge(Button::GripClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Right, d); });
			onEdge(Button::PadClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
			onEdge(Button::StickClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
			onEdge(Button::BY, [&](bool d) { io.AddKeyEvent(ImGuiKey_Tab, d); });
			onEdge(Button::AX, [&](bool d) { io.AddKeyEvent(ImGuiKey_Enter, d); });
			m_prevHeld = held;

			// Thumbstick → discrete wheel ticks.
			constexpr float kScrollAccumRate = 0.1f;
			constexpr float kScrollTickThreshold = 0.3f;
			if (std::abs(stickX) > scrollDeadzone || std::abs(stickY) > scrollDeadzone) {
				m_accumX += stickX * kScrollAccumRate;
				m_accumY += stickY * kScrollAccumRate;
				float wheelX = 0.0f, wheelY = 0.0f;
				if (std::abs(m_accumX) > kScrollTickThreshold) {
					wheelX = m_accumX > 0.0f ? 1.0f : -1.0f;
					m_accumX = 0.0f;
				}
				if (std::abs(m_accumY) > kScrollTickThreshold) {
					wheelY = m_accumY > 0.0f ? 1.0f : -1.0f;
					m_accumY = 0.0f;
				}
				if (wheelX != 0.0f || wheelY != 0.0f)
					io.AddMouseWheelEvent(-wheelX, wheelY);
			}
		}

		/// Blit the current ImGui draw data into the helper-owned panel RTV.
		/// Call after ImGui::Render(). Saves and restores the bound render
		/// target / viewport so the rest of your frame is undisturbed.
		///
		/// VR: this panel is your ONLY headset output. When connected, render to
		/// the panel and do NOT also run your normal in-game draw
		/// (ImGui_ImplDX11_RenderDrawData) into the game's frame for the VR view.
		/// If your menu hook draws into the game's HUD/menu target (e.g. Skyrim's
		/// kHUDMENU), the engine wraps that flat texture onto its CURVED world HUD
		/// and your menu comes out sheared/deformed — a second, mangled copy
		/// alongside this flat panel. Gate it: `if (IsConnected()) RenderToPanel();
		/// else ImGui_ImplDX11_RenderDrawData();`. Hooking at Present (the desktop
		/// swapchain/mirror) instead is harmless, since the mirror isn't shown in
		/// the headset.
		void RenderToPanel(ID3D11DeviceContext* ctx)
		{
			if (!IsConnected() || !ctx)
				return;

			PanelHandle panel{};
			if (!m_helper->GetPanel(m_id, &panel) || panel.rtv == nullptr)
				return;

			ImDrawData* drawData = ImGui::GetDrawData();
			if (drawData && drawData->Valid && drawData->CmdListsCount > 0) {
				BlitDrawData(ctx, panel, drawData);
				m_panelWasShowing = true;
			} else if (m_panelWasShowing) {
				// Nothing to draw this frame — clear once so the helper doesn't keep
				// compositing the last frame's pixels during an overlay swap.
				const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				ctx->ClearRenderTargetView(panel.rtv, clear);
				m_panelWasShowing = false;
			}
		}

		/// One-call interactive menu for a VR mod that brings its own ImGui context
		/// + DX11 backend (the common case). Reconciles focus against `menuOpen`,
		/// pumps controller input, and — every frame — runs NewFrame -> your draw()
		/// -> Render on the CURRENT context and blits to the panel (clearing once
		/// when hidden). `draw` only runs while shown. Returns the resolved shown
		/// state. It owns NewFrame/Render, so don't also drive them yourself that
		/// frame. For desktop mouse/keyboard you still feed ImGui yourself before
		/// this (e.g. a Win32 backend); this adds only the VR controller input —
		/// which is why a desktop+VR client with a custom frame (like Community
		/// Shaders) drives the steps individually instead of using this.
		bool RenderMenu(ID3D11DeviceContext* ctx, bool& menuOpen, const std::function<void()>& draw)
		{
			if (!IsConnected() || !ctx || !draw)
				return menuOpen;
			ReconcileFocus(menuOpen);
			const bool shown = menuOpen;
			PumpInput(shown);
			ImGui_ImplDX11_NewFrame();
			ImGui::NewFrame();
			if (shown)
				draw();
			ImGui::Render();
			RenderToPanel(ctx);  // blits when drawn, clears once when hidden
			return menuOpen;
		}

		/// Set once: loads your fonts + style into the private HUD context, called
		/// right after it's created, so RenderHud matches your desktop look.
		void SetHudStyleCallback(std::function<void()> styleSetup) { m_hudStyle = std::move(styleSetup); }

		/// Render an always-on HUD layer (a kClientFlag_HUDMode client) in one
		/// call — the whole context lifecycle the flag promises, done for you.
		/// Owns a private, non-interactive ImGui context + DX11 backend, sizes it
		/// to the (supersampled) panel so text stays crisp, re-honors window
		/// positions every frame (a passive mirror can't be dragged), runs `draw`
		/// to build the UI in your own style, blits to the panel, and clears it
		/// once when nothing is drawn. `logicalSize` is your UI's coordinate space
		/// (typically your desktop DisplaySize) so layout matches the flat screen.
		void RenderHud(ID3D11Device* device, ID3D11DeviceContext* ctx, ImVec2 logicalSize,
			const std::function<void()>& draw)
		{
			if (!IsConnected() || !device || !ctx || !draw)
				return;
			if (logicalSize.x <= 0.0f || logicalSize.y <= 0.0f)
				return;  // a zero DisplaySize would clip everything to a blank HUD

			ScopedImGuiContext guard;  // restores the caller's context on every exit

			if (!m_hudCtx) {
				m_hudCtx = ImGui::CreateContext();
				ImGui::SetCurrentContext(m_hudCtx);
				ImGuiIO& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.LogFilename = nullptr;
				if (!ImGui_ImplDX11_Init(device, ctx)) {
					ImGui::DestroyContext(m_hudCtx);
					m_hudCtx = nullptr;
					return;
				}
				if (m_hudStyle)
					m_hudStyle();
			}

			PanelHandle panel{};
			if (!m_helper->GetPanel(m_id, &panel) || panel.rtv == nullptr)
				return;

			ImGui::SetCurrentContext(m_hudCtx);
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = logicalSize;
			if (panel.width && panel.height && logicalSize.x > 0.0f && logicalSize.y > 0.0f)
				io.DisplayFramebufferScale = ImVec2(static_cast<float>(panel.width) / logicalSize.x,
					static_cast<float>(panel.height) / logicalSize.y);
			io.DeltaTime = 1.0f / 60.0f;

			ImGui_ImplDX11_NewFrame();
			ImGui::NewFrame();

			// Passive mirror: re-allow the positional Cond flags so FirstUseEver/Once
			// act like Always, and every window tracks its source position each frame.
			for (ImGuiWindow* w : m_hudCtx->Windows) {
				w->SetWindowPosAllowFlags |=
					ImGuiCond_Always | ImGuiCond_Once | ImGuiCond_FirstUseEver | ImGuiCond_Appearing;
				w->SetWindowSizeAllowFlags |=
					ImGuiCond_Always | ImGuiCond_Once | ImGuiCond_FirstUseEver | ImGuiCond_Appearing;
			}

			draw();

			ImGui::Render();
			ImDrawData* drawData = ImGui::GetDrawData();
			if (drawData && drawData->Valid && drawData->CmdListsCount > 0) {
				BlitDrawData(ctx, panel, drawData);
				m_hudWasShowing = true;
			} else if (m_hudWasShowing) {
				const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				ctx->ClearRenderTargetView(panel.rtv, clear);
				m_hudWasShowing = false;
			}
		}

		/// Destroy the private HUD context + DX11 backend (call on shutdown). Safe
		/// if RenderHud was never used.
		void ShutdownHud()
		{
			if (!m_hudCtx)
				return;
			ScopedImGuiContext guard;
			ImGui::SetCurrentContext(m_hudCtx);
			ImGui_ImplDX11_Shutdown();
			ImGui::DestroyContext(m_hudCtx);
			m_hudCtx = nullptr;
		}

		/// Forward a raw VR controller event into the helper (for clients that
		/// own a PollInputDevices-style hook). Optional.
		void FeedVREvent(uint32_t device, uint32_t key, bool pressed, float stickX, float stickY)
		{
			if (IsConnected())
				m_helper->FeedVREvent(device, key, pressed, stickX, stickY);
		}

		// ---- Bindings table widget -------------------------------------

		/// Draw a sortable / filterable table of every combo registered via
		/// AddCombo, color-coded per controller, with per-row Rebind (live
		/// capture), Clear (unbind), and — when a default was supplied to
		/// AddCombo and the binding differs from it — Reset. All three persist
		/// via your onRebind. Drop into your own settings window. `strId` must be
		/// unique if you draw more than one.
		void DrawBindingsTable(const char* strId = "##vrhelper_bindings")
		{
			if (m_combos.empty()) {
				ImGui::TextDisabled("No VR bindings registered.");
				return;
			}

			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##vrhelper_filter", "Filter bindings...", m_filter,
				sizeof(m_filter));

			const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
			                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
			if (!ImGui::BeginTable(strId, 3, flags))
				return;

			ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_DefaultSort, 0.45f);
			ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_NoSort, 0.45f);
			ImGui::TableSetupColumn("##rebind", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			// Filtered view (pointers into m_combos — stable list nodes).
			std::vector<ComboEntry*> rows;
			rows.reserve(m_combos.size());
			for (ComboEntry& e : m_combos) {
				if (m_filter[0] == '\0' || ContainsCI(e.label, m_filter))
					rows.push_back(&e);
			}

			if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
				specs && specs->SpecsCount > 0) {
				const bool ascending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
				std::sort(rows.begin(), rows.end(), [ascending](ComboEntry* a, ComboEntry* b) {
					const int cmp = CompareCI(a->label, b->label);
					return ascending ? (cmp < 0) : (cmp > 0);
				});
			}

			for (ComboEntry* e : rows) {
				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(e->id));

				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(e->label.empty() ? "(unnamed)" : e->label.c_str());

				ImGui::TableSetColumnIndex(1);
				if (e->keys.empty()) {
					ImGui::TextDisabled("(unbound)");
				} else {
					for (std::size_t i = 0; i < e->keys.size(); ++i) {
						if (i != 0) {
							ImGui::SameLine(0.0f, 0.0f);
							ImGui::TextDisabled(" + ");
							ImGui::SameLine(0.0f, 0.0f);
						}
						const InputCombo& k = e->keys[i];
						ImGui::TextColored(DeviceColor(k.GetDevice()), "%s",
							ButtonName(k.GetDevice(), k.GetKey()).c_str());
					}
				}

				ImGui::TableSetColumnIndex(2);
				if (ImGui::SmallButton("Rebind"))
					m_helper->StartComboRecording(m_id, e->label.c_str(), &RecordThunk, e, 5.0f);
				if (!e->keys.empty()) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear"))
						m_helper->RebindCombo(e->id, nullptr, 0);  // unbind (empty chord)
				}
				// Reset shown only when a factory default was provided and the
				// current binding differs from it.
				if (!e->defaults.empty() && e->keys != e->defaults) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Reset"))
						m_helper->RebindCombo(e->id, e->defaults.data(), e->defaults.size());
				}

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

	private:
		struct ComboEntry
		{
			Client* owner = nullptr;
			ComboId id = 0;
			std::string label;
			std::vector<InputCombo> keys;
			std::vector<InputCombo> defaults;  ///< factory default, for the Reset button
			RebindCallback onRebind;
		};

		static void OnFrameThunk(const Frame* f, void* user)
		{
			auto* self = static_cast<Client*>(user);
			if (!self)
				return;
			self->m_requestsRender = f && (f->flags & kFrameFlag_HasFocus) != 0;
			if (!f)
				return;
			// Pick the larger-magnitude stick axis across both hands so either
			// controller can scroll.
			const float sx = (std::abs(f->left.stick_x) >= std::abs(f->right.stick_x)) ? f->left.stick_x : f->right.stick_x;
			const float sy = (std::abs(f->left.stick_y) >= std::abs(f->right.stick_y)) ? f->left.stick_y : f->right.stick_y;
			std::scoped_lock lk{ self->m_mutex };
			self->m_heldMask = f->left.buttons_held | f->right.buttons_held;
			self->m_stickX = sx;
			self->m_stickY = sy;
		}

		// on_rebind from the helper: live keys already updated; mirror them into
		// our entry (so the table refreshes) and notify the owner to persist.
		static void RebindThunk(const InputCombo* keys, std::size_t n, void* user)
		{
			auto* e = static_cast<ComboEntry*>(user);
			if (!e)
				return;
			e->keys.assign(keys, keys + n);  // n == 0 clears (unbind)
			if (e->onRebind)
				e->onRebind(keys, n);
		}

		// Recording finished for a bindings-table Rebind button: commit via the
		// helper, which updates the live keys and fires RebindThunk.
		static void RecordThunk(const InputCombo* keys, std::size_t n, void* user)
		{
			auto* e = static_cast<ComboEntry*>(user);
			if (!e || !e->owner || !e->owner->m_helper || n == 0)
				return;
			e->owner->m_helper->RebindCombo(e->id, keys, n);
		}

		static int CompareCI(const std::string& a, const std::string& b)
		{
			const std::size_t n = std::min(a.size(), b.size());
			for (std::size_t i = 0; i < n; ++i) {
				const int ca = std::tolower(static_cast<unsigned char>(a[i]));
				const int cb = std::tolower(static_cast<unsigned char>(b[i]));
				if (ca != cb)
					return ca - cb;
			}
			return static_cast<int>(a.size()) - static_cast<int>(b.size());
		}

		static bool ContainsCI(const std::string& haystack, const char* needle)
		{
			std::string h, n;
			h.reserve(haystack.size());
			for (char c : haystack)
				h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			for (const char* p = needle; *p; ++p)
				n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
			return h.find(n) != std::string::npos;
		}

		// Blit the current draw data into `panel`, restoring the prior RT + viewport.
		// Shared by RenderToPanel and RenderHud.
		void BlitDrawData(ID3D11DeviceContext* ctx, const PanelHandle& panel, ImDrawData* drawData)
		{
			ID3D11RenderTargetView* prevRTV = nullptr;
			ID3D11DepthStencilView* prevDSV = nullptr;
			ctx->OMGetRenderTargets(1, &prevRTV, &prevDSV);

			D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ctx->RSGetViewports(&prevViewportCount, prevViewports);

			D3D11_VIEWPORT vp{};
			vp.Width = static_cast<float>(panel.width);
			vp.Height = static_cast<float>(panel.height);
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			ctx->RSSetViewports(1, &vp);

			ID3D11RenderTargetView* rtv = panel.rtv;
			ctx->OMSetRenderTargets(1, &rtv, nullptr);
			const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			ctx->ClearRenderTargetView(rtv, clearColor);

			ImGui_ImplDX11_RenderDrawData(drawData);

			ctx->OMSetRenderTargets(1, &prevRTV, prevDSV);
			if (prevViewportCount > 0)
				ctx->RSSetViewports(prevViewportCount, prevViewports);
			if (prevRTV)
				prevRTV->Release();
			if (prevDSV)
				prevDSV->Release();
		}

		IImGuiVRHelperInterface001* m_helper = nullptr;
		uint32_t m_id = 0;
		bool m_requestsRender = false;

		// OnFrame snapshot (frame thread → render thread).
		std::mutex m_mutex;
		uint32_t m_heldMask = 0;
		float m_stickX = 0.0f;
		float m_stickY = 0.0f;

		// Render-thread-only state.
		uint32_t m_prevHeld = 0;
		bool m_focusRequested = false;
		bool m_hadFocus = false;
		bool m_prevMenuOpen = false;  // ReconcileFocus edge state
		float m_accumX = 0.0f;
		float m_accumY = 0.0f;

		bool m_panelWasShowing = false;  // RenderToPanel clear-once

		// RenderHud private context: its own ImGui context + DX11 backend + style.
		ImGuiContext* m_hudCtx = nullptr;
		bool m_hudWasShowing = false;
		std::function<void()> m_hudStyle;

		std::list<ComboEntry> m_combos;  // stable node addresses back the thunk user pointers
		char m_filter[64] = {};
	};

}  // namespace ImGuiVRHelperPluginAPI
