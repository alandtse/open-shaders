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

		// Helper function to handle VR keyboard logic for text inputs
		inline bool HandleVRKeyboardForTextInput(const char* label, char* buf, size_t buf_size, bool useAdvancedActivation = false, const char* hint = nullptr)
		{
			bool changed = false;

			if (!IsVREnabled()) {
				return false;
			}

			// Static map to track previous VR text state per item
			static std::map<ImGuiID, std::string> previousVRText;
			// Static map to track initial text when keyboard was activated
			static std::map<ImGuiID, std::string> initialVRText;

			// Handle keyboard activation
			bool shouldActivateKeyboard = false;
			if (::ImGui::IsItemActivated()) {
				shouldActivateKeyboard = true;
				if (useAdvancedActivation) {
					logger::debug("VRImGui: Item activated");
				}
			} else if (useAdvancedActivation && ::ImGui::IsItemFocused() && (::ImGui::IsItemClicked() || ::ImGui::IsItemFocused())) {
				// Advanced activation logic for InputTextWithHint
				static ImGuiID lastFocusedItem = 0;
				ImGuiID currentItem = ::ImGui::GetCurrentContext()->LastItemData.ID;
				if (lastFocusedItem != currentItem) {
					shouldActivateKeyboard = true;
					lastFocusedItem = currentItem;
					if (useAdvancedActivation) {
						logger::debug("VRImGui: Item gained focus (subsequent time)");
					}
				}
			}

			if (shouldActivateKeyboard && !IsVRKeyboardVisible()) {
				std::string currentText(buf);
				const char* description = (hint && strlen(hint) > 0) ? hint : label;
				if (useAdvancedActivation) {
					logger::debug("VRImGui: Activating VR keyboard for '{}' with existing text '{}'", description, currentText);
				}
				ShowVRKeyboard(description, currentText, buf_size - 1);

				// Initialize previous VR text tracking and capture initial text
				ImGuiID currentItem = ::ImGui::GetCurrentContext()->LastItemData.ID;
				previousVRText[currentItem] = currentText;
				initialVRText[currentItem] = currentText;
			}

			// Check for involuntary keyboard close (only for advanced mode)
			if (useAdvancedActivation && !IsVRKeyboardVisible() && WasVRKeyboardClosedTooFast()) {
				logger::debug("VRImGui: Detected fast keyboard close (<100ms), forcing Tab recovery");
				ForceTabActivation();
			}

			// Handle visible keyboard text updates - inject real-time events
			if (IsVRKeyboardVisible()) {
				std::string newText = GetVRKeyboardText();
				std::string currentText(buf);
				ImGuiID currentItem = ::ImGui::GetCurrentContext()->LastItemData.ID;

				if (useAdvancedActivation) {
					logger::debug("VRImGui: VR text='{}', ImGui text='{}'", newText, currentText);
				}

				// Get previous VR text for this item
				std::string prevVRText = previousVRText.count(currentItem) ? previousVRText[currentItem] : std::string(buf);

				// Inject real-time text events if VR text changed and item is focused
				if (newText != prevVRText && ::ImGui::IsItemFocused()) {
					if (useAdvancedActivation) {
						logger::debug("VRImGui: VR text changed from '{}' to '{}', injecting events", prevVRText, newText);
					}

					// Calculate the difference and inject appropriate events
					if (newText.length() < prevVRText.length()) {
						// Text was shortened - inject backspace events
						size_t deleteCount = prevVRText.length() - newText.length();
						for (size_t i = 0; i < deleteCount; i++) {
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
						}
						if (useAdvancedActivation) {
							logger::debug("VRImGui: Injected {} backspace events", deleteCount);
						}
					} else if (newText.length() > prevVRText.length()) {
						// Text was extended - inject new characters
						std::string addedText = newText.substr(prevVRText.length());
						for (char c : addedText) {
							if (c != '\0') {
								::ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c));
							}
						}
						if (useAdvancedActivation) {
							logger::debug("VRImGui: Injected {} new characters: '{}'", addedText.length(), addedText);
						}
					} else if (newText != prevVRText) {
						// Same length but different content - replace by selecting all and retyping
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, true);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
						::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, false);

						for (char c : newText) {
							if (c != '\0') {
								::ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c));
							}
						}
						if (useAdvancedActivation) {
							logger::debug("VRImGui: Replaced text content via select-all + retype");
						}
					}

					// Update previous VR text
					previousVRText[currentItem] = newText;
					changed = true;
				}
				// Remove unfocused item updates - VR keyboard should only affect focused items
			}

			// Handle finished keyboard - use definitive text for final correction
			if (IsVRKeyboardFinished()) {
				std::string finalText = GetVRKeyboardText();
				std::string currentText(buf);
				ImGuiID currentItem = ::ImGui::GetCurrentContext()->LastItemData.ID;

				// Bug fix: If VR keyboard finished with a single character AND we started with longer text,
				// assume it's an incomplete deletion that should be treated as a clear event (empty string)
				std::string startingText = initialVRText.count(currentItem) ? initialVRText[currentItem] : "";
				if (finalText.length() == 1 && startingText.length() > 1) {
					finalText = "";
					if (useAdvancedActivation) {
						logger::debug("VRImGui: Final VR text is single character but started with '{}' (length {}), treating as incomplete deletion", startingText, startingText.length());
					}
				}

				if (useAdvancedActivation) {
					logger::debug("VRImGui: Keyboard finished - final VR text='{}', ImGui text='{}'", finalText, currentText);
				}

				// Use definitive VR text for final correction if there's a mismatch
				if (finalText != currentText) {
					if (::ImGui::IsItemFocused()) {
						if (useAdvancedActivation) {
							logger::debug("VRImGui: Final correction - syncing ImGui to definitive VR text '{}'", finalText);
						}

						// If final text is empty and we have text to clear, just inject one backspace
						if (finalText.empty() && !currentText.empty()) {
							// Simple backspace to clear the remaining character
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
							if (useAdvancedActivation) {
								logger::debug("VRImGui: Cleared remaining text with single backspace");
							}
						} else {
							// For other cases, use select-all + retype
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, true);
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
							::ImGui::GetIO().AddKeyEvent(ImGuiKey_ModCtrl, false);

							// Inject final definitive text character by character (or leave empty if finalText is empty)
							if (!finalText.empty()) {
								for (char c : finalText) {
									if (c != '\0') {
										::ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c));
									}
								}
							}
							if (useAdvancedActivation) {
								logger::debug("VRImGui: Final definitive text '{}' injected via select-all + retype", finalText);
							}
						}
						changed = true;
					}
					// Remove unfocused item updates - VR keyboard should only affect focused items
				}

				// Update tracking and clear finished flag
				previousVRText[currentItem] = finalText;
				// Clear initial text tracking since keyboard session is done
				initialVRText.erase(currentItem);
				ClearVRKeyboardFinished();
			}

			return changed;
		}
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
			bool vrChanged = Internal::HandleVRKeyboardForTextInput(label, buf, buf_size, false, nullptr);
			changed = changed || vrChanged;
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
			bool vrChanged = Internal::HandleVRKeyboardForTextInput(label, buf, buf_size, true, hint);
			changed = changed || vrChanged;
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
			bool vrChanged = Internal::HandleVRKeyboardForTextInput(label, buf, buf_size, false, nullptr);
			changed = changed || vrChanged;
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
