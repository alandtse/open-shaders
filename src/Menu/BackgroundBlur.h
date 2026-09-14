#pragma once

#include <d3d11.h>
#include <mutex>
#include <winrt/base.h>

namespace BackgroundBlur
{
	/**
	 * @brief Initializes blur shaders and GPU resources
	 * @return True if initialization succeeded
	 */
	bool Initialize();

	/**
	 * @brief Renders background blur behind all visible ImGui windows
	 * This is the main entry point - call after ImGui::Render() but before ImGui_ImplDX11_RenderDrawData()
	 */
	void RenderBackgroundBlur();

	/**
	 * @brief Cleans up all blur resources
	 */
	void Cleanup();

	void SetEnabled(bool enable);

	/** @brief Enables fullscreen scene blur with separate blur for overlapping editor windows. */
	void SetCSEditorActive(bool active);
	bool IsCSEditorActive();

}  // namespace BackgroundBlur
