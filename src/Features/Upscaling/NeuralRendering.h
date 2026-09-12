#pragma once

#include "NeuralRendering/Tuning.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/** @brief Applies one native-resolution NGX Neural Rendering pass before upscaling. */
struct NeuralRendering
{
	NeuralRendering();
	~NeuralRendering();
	/** @brief Schedules recreation on the rendering thread. */
	void SetupResources();
	/** @brief Invalidates both temporal histories on loading or setting changes. */
	void ResetHistory();
	/** @brief Discards history when NR or the world is inactive. */
	void Reset(bool enabled);
	/** @brief Invalidates the cached existing upscaling encoder. */
	void ClearShaderCache();
	/** @brief Draws Upscaling's NR tuning, retry controls, and runtime status. */
	void DrawSettings(bool& enabled, NR::Tuning& tuning);
	/** @brief Replaces active kMAIN eye regions before the OS upscale/post chain. */
	void DrawBeforeUpscaling(bool enabled, const NR::Tuning& tuning);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	std::atomic_bool resetHistory = true, recreate = false, clearShaders = false, retryRequested = false;
	std::mutex statusMutex;
	std::string status = "Disabled";
	void SetStatus(std::string message);
};
