#include "HomePageRenderer.h"
#include "PCH.h"

#include <imgui.h>

#include "Globals.h"
#include "Menu.h"
#include "Plugin.h"
#include "State.h"
#include "Util.h"

// Static member definitions
bool HomePageRenderer::isFirstTimeSetupShown = false;

void HomePageRenderer::RenderHomePage()
{
	ImGui::BeginChild("HomePage", ImVec2(0, 0), false);

	RenderWelcomeSection();
	ImGui::Spacing();

	// RenderQuickLinksSection();
	// ImGui::Spacing();

	// RenderFAQSection();

	ImGui::EndChild();
}

void HomePageRenderer::RenderWelcomeSection()
{
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));

	// Main title - centered with safe font handling
	ImGuiIO& io = ImGui::GetIO();
	ImFont* titleFont = nullptr;

	// Safely check if we have multiple fonts and the second one is valid
	if (io.Fonts && io.Fonts->Fonts.Size > 1 && io.Fonts->Fonts[1] != nullptr) {
		titleFont = io.Fonts->Fonts[1];
	}

	// Scale the text to make it larger (2.0x size)
	ImGui::SetWindowFontScale(TITLE_FONT_SCALE);

	// Only push font if we have a valid one, otherwise use default scaled
	if (titleFont) {
		ImGui::PushFont(titleFont);
	}

	// (If you want a big title text, insert it here before PopFont)

	// Only pop font if we pushed one
	if (titleFont) {
		ImGui::PopFont();
	}

	// Reset text scale back to normal
	ImGui::SetWindowFontScale(1.0f);

	ImGui::Spacing();

	// We use windowSize below, so define it here
	ImVec2 windowSize = ImGui::GetWindowSize();

	// Intro text - centered
	const char* introText =
		"This is an unofficial fork of Community Shaders restoring Particle Lights.\n"
		"               Not affiliated with or endorsed by the Community Shaders team\n"
		"      - Visit their Discord to get the Original and support their outstanding efforts -";
	ImVec2 introSize = ImGui::CalcTextSize(introText);
	ImGui::SetCursorPosX((windowSize.x - introSize.x) * 0.5f);
	ImGui::TextWrapped("%s", introText);

	ImGui::Spacing();

	// Extra vertical padding - move banner down by 40.0f (pixels)
	ImGui::Dummy(ImVec2(0.0f, 50.0f));

	// Discord banner - centered with proper error checking
	auto menu = Menu::GetSingleton();
	bool discordIconAvailable = false;

	// Check if menu exists, has icons, and Discord icon is loaded
	if (menu && menu->uiIcons.discord.texture != nullptr &&
		menu->uiIcons.discord.size.x > 0 && menu->uiIcons.discord.size.y > 0) {
		discordIconAvailable = true;
	}

	if (discordIconAvailable) {
		// Calculate scaled icon size based on window width, with min/max constraints
		ImVec2 originalSize = ImVec2(menu->uiIcons.discord.size.x, menu->uiIcons.discord.size.y);

		// Compute width based on window size with constraints and padding (handles very small windows)
		float ratioWidth = windowSize.x * DISCORD_BANNER_TARGET_WIDTH_RATIO;
		float aspectRatio = originalSize.y / originalSize.x;
		float maxAllowed = std::max(1.0f, windowSize.x - DISCORD_BANNER_PADDING_MARGIN);
		float upperBound = std::min(DISCORD_BANNER_MAX_WIDTH, maxAllowed);
		float lowerBound = std::min(DISCORD_BANNER_MIN_WIDTH, upperBound);
		float targetWidth = std::clamp(ratioWidth, lowerBound, upperBound);

		ImVec2 iconSize = ImVec2(targetWidth, targetWidth * aspectRatio);
		ImGui::SetCursorPosX((windowSize.x - iconSize.x) * 0.5f);

		// Purely decorative: draw the banner image only (no button, no click, no tooltip)
		ImGui::Image(menu->uiIcons.discord.texture, iconSize);
	} else {
		// No Discord icon available: keep layout roughly consistent with a dummy spacer,
		// but do not show a clickable button or link.
		float dummyWidth = 200.0f;
		ImGui::SetCursorPosX((windowSize.x - dummyWidth) * 0.5f);
		ImGui::Dummy(ImVec2(dummyWidth, 0.0f));
	}

	// Pop the style var we pushed at the start
	ImGui::PopStyleVar();
	// Close RenderWelcomeSection()
}

void HomePageRenderer::RenderFirstTimeSetupDialog()
{
	// Block input to the game and make cursor visible - input blocking is handled by ShouldSwallowInput()
	auto& io = ImGui::GetIO();
	io.WantCaptureMouse = true;
	io.WantCaptureKeyboard = true;
	io.MouseDrawCursor = true;  // Show ImGui cursor

	// Center the window properly with rounded corners and thin border
	ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	// Set a minimum width for better layout, but allow auto-sizing for height
	ImGui::SetNextWindowSizeConstraints(ImVec2(500, 0), ImVec2(600, FLT_MAX));

	// Style for rounded window with thin border
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
	                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize;

	if (!ImGui::Begin("##FirstTimeSetup", nullptr, flags)) {
		ImGui::PopStyleVar(2);
		ImGui::End();
		return;
	}

	// Set absolute font size for better readability in this welcome dialog
	float targetFontSize = 27.0f;
	float currentFontSize = io.FontDefault ? io.FontDefault->FontSize : io.FontGlobalScale * 13.0f;
	float fontScale = targetFontSize / currentFontSize;
	ImGui::SetWindowFontScale(fontScale);

	auto menu = Menu::GetSingleton();

	// Render CS logo as background watermark with proper aspect ratio
	if (menu && menu->uiIcons.logo.texture) {
		ImVec2 windowPos = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();

		// Get the original texture size to maintain aspect ratio
		ImVec2 textureSize = menu->uiIcons.logo.size;
		float aspectRatio = textureSize.x / textureSize.y;

		// Set desired height and calculate width to maintain aspect ratio
		float logoHeight = LOGO_WATERMARK_HEIGHT;
		float logoWidth = logoHeight * aspectRatio;

		ImVec2 logoMin(windowPos.x + (windowSize.x - logoWidth) * 0.5f,
			windowPos.y + (windowSize.y - logoHeight) * 0.5f);
		ImVec2 logoMax(logoMin.x + logoWidth, logoMin.y + logoHeight);

		// Determine watermark color based on monochrome logo setting
		ImU32 watermarkColor;
		if (menu->GetSettings().Theme.UseMonochromeLogo) {
			ImVec4 textColor = menu->GetSettings().Theme.Palette.Text;
			textColor.w = 0.24f;  // Low alpha for watermark effect
			watermarkColor = ImGui::GetColorU32(textColor);
		} else {
			watermarkColor = IM_COL32(255, 255, 255, 60);
		}

		// Render as subtle watermark background
		ImGui::GetWindowDrawList()->AddImage(menu->uiIcons.logo.texture, logoMin, logoMax,
			ImVec2(0, 0), ImVec2(1, 1), watermarkColor);
	}

	// Center all content
	float windowWidth = ImGui::GetWindowWidth();

	// Welcome title - centered
	const char* welcomeTitle = "Welcome to Community Shaders - Particle Lights (Unofficial Fork)!";
	float welcomeTitleWidth = ImGui::CalcTextSize(welcomeTitle).x;
	ImGui::SetCursorPosX((windowWidth - welcomeTitleWidth) * 0.5f);
	ImGui::Text("%s", welcomeTitle);

	ImGui::Spacing();

	// Version text - wrapped and centered
	const char* versionText = "This appears to be a new install, update, or reinstallation of Community Shaders.";
	float textPadding = 40.0f;  // Padding from window edges

	// Use a centered region for wrapped text
	ImGui::SetCursorPosX(textPadding);
	ImGui::BeginGroup();
	ImGui::PushTextWrapPos(windowWidth - textPadding);

	// Calculate the wrapped text size to center it
	ImVec2 textSize = ImGui::CalcTextSize(versionText, nullptr, true, windowWidth - textPadding * 2);
	float centerOffset = (windowWidth - textPadding * 2 - textSize.x) * 0.5f;
	if (centerOffset > 0) {
		ImGui::SetCursorPosX(textPadding + centerOffset);
	}

	ImGui::TextWrapped("%s", versionText);
	ImGui::PopTextWrapPos();
	ImGui::EndGroup();

	ImGui::Spacing();

	// Description - centered
	const char* description = "Please select a hotkey to access the menu:";
	float descWidth = ImGui::CalcTextSize(description).x;
	ImGui::SetCursorPosX((windowWidth - descWidth) * 0.5f);
	ImGui::Text("%s", description);

	// Hotkey selection - clickable hotkey text
	// Show current toggle key and allow user to change it by clicking on it
	auto& themeSettings = menu->GetTheme();
	const char* currentKeyName = Util::Input::KeyIdToString(menu->GetSettings().ToggleKey);

	// Increase font size for hotkey text
	ImGui::SetWindowFontScale(fontScale * HOTKEY_TEXT_SCALE_MULTIPLIER);

	// Calculate text dimensions for centering and button area
	float hotkeyWidth = ImGui::CalcTextSize(currentKeyName).x;
	float centerX = (windowWidth - hotkeyWidth) * 0.5f;
	ImGui::SetCursorPosX(centerX);

	// Create invisible button for hover detection and clicking
	ImVec2 buttonPos = ImGui::GetCursorScreenPos();
	ImVec2 hotkeyTextSize = ImGui::CalcTextSize(currentKeyName);
	bool hovered = false;
	bool clicked = false;

	ImGui::PushID("HotkeyButton");
	if (ImGui::InvisibleButton("##HotkeyClick", hotkeyTextSize)) {
		clicked = true;
	}
	hovered = ImGui::IsItemHovered();
	ImGui::PopID();

	// Set cursor position back for text rendering
	ImGui::SetCursorScreenPos(buttonPos);

	// Choose color based on hover state - darken when hovered.
	ImVec4 hotkeyColor = hovered ?
	                         ImVec4(themeSettings.StatusPalette.CurrentHotkey.x * 0.7f,
								 themeSettings.StatusPalette.CurrentHotkey.y * 0.7f,
								 themeSettings.StatusPalette.CurrentHotkey.z * 0.7f,
								 themeSettings.StatusPalette.CurrentHotkey.w) :
	                         themeSettings.StatusPalette.CurrentHotkey;

	ImGui::TextColored(hotkeyColor, "%s", currentKeyName);

	// Reset font scale
	ImGui::SetWindowFontScale(fontScale);

	// Handle click to start hotkey capture
	if (clicked) {
		menu->settingToggleKey = true;
	}

	// Show hotkey capture message or hotkey text
	if (menu->settingToggleKey) {
		const char* pressKeyText = "Press any key to set as toggle key...";
		float pressKeyWidth = ImGui::CalcTextSize(pressKeyText).x;
		ImGui::SetCursorPosX((windowWidth - pressKeyWidth) * 0.5f);
		ImGui::Text("%s", pressKeyText);
	}

	ImGui::Spacing();

	// "You can change this later" text - wrapped and centered
	const char* laterText = "You can change this later in General > Keybindings.";
	float laterWidth = ImGui::CalcTextSize(laterText).x;
	if (laterWidth > windowWidth - 40.0f) {
		// Text is too wide, use wrapped text with centering
		float laterTextPadding = 40.0f;

		ImGui::SetCursorPosX(laterTextPadding);
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(windowWidth - laterTextPadding);

		// Calculate the wrapped text size to center it
		ImVec2 laterTextSize = ImGui::CalcTextSize(laterText, nullptr, true, windowWidth - laterTextPadding * 2);
		float laterCenterOffset = (windowWidth - laterTextPadding * 2 - laterTextSize.x) * 0.5f;
		if (laterCenterOffset > 0) {
			ImGui::SetCursorPosX(laterTextPadding + laterCenterOffset);
		}

		ImGui::TextWrapped("%s", laterText);
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
	} else {
		// Text fits, center it normally
		ImGui::SetCursorPosX((windowWidth - laterWidth) * 0.5f);
		ImGui::Text("%s", laterText);
	}

	ImGui::Spacing();

	// Check for Enter or Escape key to close, but only if not capturing a hotkey
	bool shouldClose = (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape)) && !menu->settingToggleKey;

	if (shouldClose) {
		MarkFirstTimeSetupComplete();
		// Note: Settings are automatically saved to ensure welcome screen won't show again
	}

	// Center the help text
	const char* helpText = "Press Escape or Enter to continue";
	float helpWidth = ImGui::CalcTextSize(helpText).x;
	ImGui::SetCursorPosX((windowWidth - helpWidth) * 0.5f);
	ImGui::TextDisabled("%s", helpText);

	ImGui::End();
	ImGui::PopStyleVar(2);
}

bool HomePageRenderer::ShouldShowFirstTimeSetup()
{
	// Never show first-time setup in VR mode
	if (REL::Module::IsVR()) {
		return false;
	}

	// Check if already completed this session
	if (isFirstTimeSetupShown) {
		return false;
	}

	// Check if first-time setup has been completed using the Menu settings
	auto menu = Menu::GetSingleton();
	return !menu->GetSettings().FirstTimeSetupCompleted;
}

void HomePageRenderer::MarkFirstTimeSetupComplete()
{
	// Set the flag in the Menu settings
	auto menu = Menu::GetSingleton();
	menu->GetSettings().FirstTimeSetupCompleted = true;

	// Immediately save settings to ensure the flag is persisted
	// This prevents the welcome screen from showing again even if user doesn't manually save
	globals::state->Save();

	isFirstTimeSetupShown = true;  // Mark as shown this session
}
