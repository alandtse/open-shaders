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
//   // at SKSE kPostLoad:
//   g_vr.Connect("MyMod", versionStr, ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus);
//   g_vr.AddCombo("Open menu", myOpenKeys, [](auto* k, auto n){ /* persist */ });
//   // each render frame, after building your ImGui frame and calling ImGui::Render():
//   if (g_vr.Fired(openCombo)) menuOpen = true;
//   if (g_vr.ConsumeFocusLost()) menuOpen = false;
//   g_vr.SyncFocus(menuOpen);
//   g_vr.PumpInput(menuOpen || g_vr.HasFocus());
//   g_vr.RenderToPanel(myD3DContext);
//
// Plus a drop-in, sortable/filterable bindings table for your own settings UI:
//
//   g_vr.DrawBindingsTable();
//
// This header pulls in <imgui.h>, <imgui_impl_dx11.h> and <d3d11.h>; include
// it from a translation unit that already has ImGui + the DX11 backend. It
// does NOT depend on CommonLibSSE/RE — button names use the OpenVR key codes
// directly — so it is safe to vendor alongside the other api/ headers.

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

	/// Convenience wrapper around the helper interface. One instance per mod;
	/// not thread-safe except for the OnFrame snapshot, which is mutex-guarded
	/// so the helper's frame thread and your render thread can share it.
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

		/// Perform the kPostLoad handshake and register as a client. Safe to
		/// call once; returns true if the helper is installed and registration
		/// succeeded. `flags` is a bitmask of kClientFlag_*.
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
		void SyncFocus(bool menuOpen)
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
		bool ConsumeFocusLost()
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
		void RenderToPanel(ID3D11DeviceContext* ctx)
		{
			if (!IsConnected() || !ctx)
				return;

			PanelHandle panel{};
			if (!m_helper->GetPanel(m_id, &panel) || panel.rtv == nullptr)
				return;

			ImDrawData* drawData = ImGui::GetDrawData();
			if (!drawData || !drawData->Valid || drawData->CmdListsCount <= 0)
				return;

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
		float m_accumX = 0.0f;
		float m_accumY = 0.0f;

		std::list<ComboEntry> m_combos;  // stable node addresses back the thunk user pointers
		char m_filter[64] = {};
	};

}  // namespace ImGuiVRHelperPluginAPI
