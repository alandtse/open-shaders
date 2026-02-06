#pragma once

class HomePageRenderer
{
public:
	// Font scales
	static constexpr float TITLE_FONT_SCALE = 2.0f;
	static constexpr float SETUP_DIALOG_FONT_SCALE = 0.75f;
	static constexpr float HOTKEY_TEXT_SCALE_MULTIPLIER = 1.2f;

	// Logo watermark height (in pixels)
	static constexpr float LOGO_WATERMARK_HEIGHT = 260.0f;

	// Banner scaling constants
	// Uses window width * DISCORD_BANNER_TARGET_WIDTH_RATIO, then clamped --> Discord banner replaced with 
	static constexpr float DISCORD_BANNER_TARGET_WIDTH_RATIO = 0.85f;  // 85% of window width
	static constexpr float DISCORD_BANNER_MIN_WIDTH = 150.0f;
	static constexpr float DISCORD_BANNER_MAX_WIDTH = 200.0f;
	static constexpr float DISCORD_BANNER_PADDING_MARGIN = 40.0f;

	static void RenderHomePage();

	// First-time setup management
	static bool ShouldShowFirstTimeSetup();
	static void RenderFirstTimeSetupDialog();
	static bool ShouldSkipKeyRelease(uint32_t key);

private:
	static void RenderWelcomeSection();
	static void MarkFirstTimeSetupComplete(uint32_t closingKey);

	// State
	static bool isFirstTimeSetupShown;
	static uint32_t keyThatClosedDialog;
};
