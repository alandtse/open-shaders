#pragma once

#include <d3d11.h>
#include <mutex>
#include <winrt/base.h>

struct ImDrawData;
struct ImVec2;

namespace BackgroundBlur
{
	/**
	 * @brief Initializes blur shaders and GPU resources
	 * @return True if initialization succeeded
	 */
	bool Initialize();

	/** @brief Renders ImGui with ordered background blur; returns false when the caller must render normally. */
	bool RenderDrawData(ImDrawData* drawData);
	/** @brief Restores blur-modified buffers only when the engine has retained their previous contents. */
	void RestoreRetainedBuffers();
	/** @brief Draws an opaque HDR scene preview without passing scene color through the SDR UI buffer. */
	bool ImageHDRScene(const ImVec2& size);

	/**
	 * @brief Cleans up all blur resources
	 */
	void Cleanup();

	void SetEnabled(bool enable);

	/** @brief Enables fullscreen scene blur with separate blur for overlapping editor windows. */
	void SetCSEditorActive(bool active);
	bool IsCSEditorActive();

}  // namespace BackgroundBlur
