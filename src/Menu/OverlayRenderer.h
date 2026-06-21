#pragma once

#include "ImGuiVRHelperClient.h"  // ImGuiVRHelperClient::Surface
#include "Utils/Input.h"
#include <functional>
#include <vector>

class Menu;

/**
 * @brief Specialized renderer component for overlay and frame management
 *
 * This class was extracted from Menu.cpp to handle all overlay-related rendering
 * responsibilities including VR setup, shader compilation status, feature overlays,
 * A/B testing, and ImGui frame lifecycle management.
 *
 * The renderer uses a callback-based architecture to maintain separation of concerns
 * while accessing necessary Menu functionality through provided function objects.
 *
 * @note This class is designed as a stateless utility with static methods to avoid
 *       coupling to Menu's internal state while providing clean separation of concerns.
 */
class OverlayRenderer
{
public:
	/**
	 * @brief Main overlay rendering entry point
	 *
	 * Coordinates all overlay rendering activities including VR setup, input processing,
	 * shader compilation status display, feature overlays, A/B testing, and ImGui frame
	 * management. Uses callback functions to access Menu functionality while maintaining
	 * architectural separation.
	 *
	 * @param menu Reference to the Menu instance for accessing settings and state
	 * @param processInputEventQueue Callback to process queued input events
	 * @param drawSettings Callback to render the main settings interface
	 * @param keyIdToString Callback to convert key codes to human-readable strings
	 * @param cachedFontSize Current cached font size for reload detection
	 * @param currentFontSize Current font size setting for comparison
	 */
	static void RenderOverlay(
		Menu& menu,
		const std::function<void()>& processInputEventQueue,
		const std::function<void()>& drawSettings,
		const std::function<const char*(std::vector<InputCombo>)>& keyIdToString,
		float& cachedFontSize,
		float currentFontSize);

private:
	static bool ShouldSkipRendering();
	static void HandleFontReload(Menu& menu, float& cachedFontSize, float currentFontSize);
	static void InitializeImGuiFrame(Menu& menu);
	static void RenderShaderCompilationStatus(const std::function<const char*(std::vector<InputCombo>)>& keyIdToString);
	static void RenderShaderBlockingStatus();
	static void RenderFirstTimeSetupOverlay();
	static void RenderFeatureOverlays();
	static void RenderWelcomeOverlay();
	static void HandleABTesting();
	static void FinalizeImGuiFrame();

	// Two-pass VR rendering (used only when the helper is registered).
	// The MENU pass runs first so its ImGui::NewFrame() consumes the
	// wand/keyboard input queued by ImGuiVRHelperClient::Update(); the HUD
	// pass is non-interactive and gets no input.
	static void RenderMenuPass(
		Menu& menu,
		const std::function<void()>& drawSettings,
		const std::function<const char*(std::vector<InputCombo>)>& keyIdToString);
	static void RenderHudPass(
		const std::function<const char*(std::vector<InputCombo>)>& keyIdToString);

	// MENU-pass frame lifecycle (the original Initialize/Finalize split,
	// but Finalize now targets a specified surface).
	static void BeginMenuPass(Menu& menu);
	static void EndMenuPass(ImGuiVRHelperClient::Surface surface);

	// HUD-pass frame lifecycle: a lighter NewFrame that does NOT re-run
	// ImGui_ImplWin32_NewFrame (no double time-advance / desktop-cursor
	// read) and feeds no input.
	static void BeginHudPass();
	static void EndHudPass(ImGuiVRHelperClient::Surface surface);
};