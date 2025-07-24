#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <map>
#include <string>

/**
 * @file VRImGui.h
 * @brief Transparent VR keyboard support for ImGui text input functions
 *
 * This header provides drop-in replacements for ImGui text input functions that
 * automatically show the VR virtual keyboard when clicked in VR mode, while
 * behaving exactly like normal ImGui functions in non-VR mode.
 *
 * Usage: Just include this header and use the normal ImGui function names.
 * The VR keyboard support will be completely transparent.
 *
 */

// VR keyboard support - always compiled but runtime-gated
namespace VRImGui
{
	// Internal VR keyboard interface - don't call directly
	namespace Internal
	{
		bool ShowVRKeyboard(const char* description, const std::string& existingText, size_t maxLength = 256, bool useMinimalMode = false);
		bool IsVRKeyboardVisible();
		std::string GetVRKeyboardText();
		void ProcessVRKeyboardEvents();
		bool IsVREnabled();
		bool IsVRKeyboardFinished();
		void ClearVRKeyboardFinished();
		bool WasVRKeyboardClosedTooFast();
		void ForceTabActivation();
	}

	/**
     * VR-enhanced ImGui::InputText that automatically shows virtual keyboard when clicked in VR
     */
	inline bool InputText(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr)
	{
		bool changed = false;

		// Standard ImGui input text
		bool isActive = ::ImGui::InputText(label, buf, buf_size, flags, callback, user_data);
		changed = isActive;

		// VR keyboard support (only in VR mode)
		if (Internal::IsVREnabled()) {
			// If the input field was just activated (clicked), show VR keyboard
			if (::ImGui::IsItemActivated()) {
				std::string currentText(buf);
				if (Internal::ShowVRKeyboard(label, currentText, buf_size - 1)) {
					// Keyboard shown successfully
				}
			}

			// If VR keyboard is visible and has updated text, apply it to the buffer
			if (Internal::IsVRKeyboardVisible()) {
				std::string newText = Internal::GetVRKeyboardText();
				if (newText != std::string(buf)) {
					size_t copyLen = std::min(newText.length(), buf_size - 1);
					std::memcpy(buf, newText.c_str(), copyLen);
					buf[copyLen] = '\0';
					changed = true;
				}
			}
		}

		return changed;
	}

	/**
     * VR-enhanced ImGui::InputTextWithHint
     */
	inline bool InputTextWithHint(const char* label, const char* hint, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr)
	{
		bool changed = false;

		// Standard ImGui input text with hint
		bool isActive = ::ImGui::InputTextWithHint(label, hint, buf, buf_size, flags, callback, user_data);
		changed = isActive;

		// VR keyboard support (only in VR mode)
		if (Internal::IsVREnabled()) {
			// Check for VR keyboard activation - detect focus changes instead of just clicks
			bool shouldActivateKeyboard = false;
			if (::ImGui::IsItemActivated()) {
				// Initial activation (first time or via Tab/Shift+Tab)
				shouldActivateKeyboard = true;
				logger::debug("VRImGui InputTextWithHint: Item activated");
			} else if (::ImGui::IsItemFocused() && (::ImGui::IsItemClicked() || ::ImGui::IsItemFocused())) {
				// If item is focused and was clicked, OR if it just gained focus
				static ImGuiID lastFocusedItem = 0;
				ImGuiID currentItem = ::ImGui::GetCurrentContext()->LastItemData.ID;
				if (lastFocusedItem != currentItem) {
					// This item just gained focus
					shouldActivateKeyboard = true;
					lastFocusedItem = currentItem;
					logger::debug("VRImGui InputTextWithHint: Item gained focus (subsequent time)");
				}
			}

			if (shouldActivateKeyboard && !Internal::IsVRKeyboardVisible()) {
				std::string currentText(buf);
				// Use hint as description if available, otherwise use label
				const char* description = hint && strlen(hint) > 0 ? hint : label;
				logger::debug("VRImGui InputTextWithHint: Activating VR keyboard for '{}' with existing text '{}'", description, currentText);
				Internal::ShowVRKeyboard(description, currentText, buf_size - 1);
			}

			// Check for involuntary keyboard close (under 100ms) and recover with Tab activation
			if (!Internal::IsVRKeyboardVisible() && Internal::WasVRKeyboardClosedTooFast()) {
				logger::debug("VRImGui InputTextWithHint: Detected fast keyboard close (<100ms), forcing Tab recovery");
				Internal::ForceTabActivation();
			}

			// If VR keyboard is visible and has updated text, apply it to the buffer
			if (Internal::IsVRKeyboardVisible()) {
				std::string newText = Internal::GetVRKeyboardText();
				std::string currentText(buf);

				logger::debug("VRImGui InputTextWithHint: Comparing VR text='{}' with ImGui text='{}'", newText, currentText);

				// Simple logic: if VR keyboard text differs from ImGui buffer, inject it
				if (newText != currentText) {
					logger::debug("VRImGui InputTextWithHint: VR text differs from ImGui, manually injecting complete text '{}'", newText);

					if (::ImGui::IsItemFocused()) {
						// Clear existing content first with Ctrl+A and then inject new text
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, true);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, false);

						// Inject the complete new text character by character
						for (char c : newText) {
							if (c != '\0') {
								::ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c));
							}
						}
						changed = true;

						logger::debug("VRImGui InputTextWithHint: Complete text manually injected via key events");
					}
				}
			}

			// Handle finished keyboard - do final sync and cleanup
			if (Internal::IsVRKeyboardFinished()) {
				std::string finalText = Internal::GetVRKeyboardText();
				std::string currentText(buf);

				if (finalText != currentText) {
					logger::debug("VRImGui InputTextWithHint: Final VR text injection '{}'", finalText);

					// Use complete manual injection for final text as well
					if (::ImGui::IsItemFocused()) {
						// Clear existing content first with Ctrl+A
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, true);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, false);

						// Inject final text character by character (or leave empty if finalText is empty)
						if (!finalText.empty()) {
							for (char c : finalText) {
								if (c != '\0') {
									::ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c));
								}
							}
						}
						changed = true;

						logger::debug("VRImGui InputTextWithHint: Final text completely injected via key events");
					}
				}
				// Clear the finished flag after processing
				Internal::ClearVRKeyboardFinished();
			}
		}

		return changed;
	}

	/**
     * VR-enhanced ImGui::InputTextMultiline
     */
	inline bool InputTextMultiline(const char* label, char* buf, size_t buf_size, const ImVec2& size = ImVec2(0, 0), ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr)
	{
		bool changed = false;

		// Standard ImGui multiline input text
		bool isActive = ::ImGui::InputTextMultiline(label, buf, buf_size, size, flags, callback, user_data);
		changed = isActive;

		// VR keyboard support (only in VR mode)
		if (Internal::IsVREnabled()) {
			// If the input field was just activated (clicked), show VR keyboard
			if (::ImGui::IsItemActivated()) {
				std::string currentText(buf);
				if (Internal::ShowVRKeyboard(label, currentText, buf_size - 1)) {
					// Keyboard shown successfully
				}
			}

			// If VR keyboard is visible and has updated text, apply it to the buffer
			if (Internal::IsVRKeyboardVisible()) {
				std::string newText = Internal::GetVRKeyboardText();
				if (newText != std::string(buf)) {
					size_t copyLen = std::min(newText.length(), buf_size - 1);
					std::memcpy(buf, newText.c_str(), copyLen);
					buf[copyLen] = '\0';
					changed = true;
				}
			}
		}

		return changed;
	}

	/**
     * VR-enhanced InputText for std::string
     */
	inline bool InputText(const char* label, std::string& str, ImGuiInputTextFlags flags = 0)
	{
		// Use a buffer large enough for most text inputs
		char buffer[1024];
		size_t copyLen = std::min(str.length(), sizeof(buffer) - 1);
		std::memcpy(buffer, str.c_str(), copyLen);
		buffer[copyLen] = '\0';

		bool changed = InputText(label, buffer, sizeof(buffer), flags);

		if (changed) {
			str = buffer;
		}

		return changed;
	}
}

// Transparent macros that replace ImGui functions with VR-enabled versions
#define ImGui_InputText VRImGui::InputText
#define ImGui_InputTextWithHint VRImGui::InputTextWithHint
#define ImGui_InputTextMultiline VRImGui::InputTextMultiline
