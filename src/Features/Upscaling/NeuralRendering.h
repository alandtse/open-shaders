#pragma once

#include "NeuralRendering/Diagnostics.h"
#include "NeuralRendering/Runtime.h"
#include "NeuralRendering/Tuning.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/** @brief Applies one native-resolution NGX Neural Rendering pass before upscaling. */
struct NeuralRendering
{
	/** @brief What NR is doing, as the settings panel and devbench report it. */
	struct Status
	{
		enum class State : uint8_t
		{
			kOff,       ///< Switched off.
			kStarting,  ///< Enabled; the runtime initializes on the first world frame.
			kActive,    ///< Running; text names the runtime version.
			kFailed     ///< Latched; text is the failure reason.
		};
		State state = State::kOff;
		/** @brief Plain-language line for the settings panel and the devbench query; empty until the first publish. */
		std::string text;
		/** @brief Accepted nvngx_dlssnr.dll version; empty until the runtime initializes. */
		std::string runtimeVersion;
		uint32_t width = 0, height = 0, eyes = 0;
		uint32_t lastAppliedFrame = UINT32_MAX, appliedFrames = 0;
		std::array<uint32_t, 2> ngxResult{};
		/** @brief True while a failure is latched; the next successful frame clears it. */
		bool failed = false;
	};

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
	/** @brief Replaces active kMAIN eye regions before upscaling and frame-generation capture. */
	void DrawBeforeUpscaling(bool enabled, const NR::Tuning& tuning, uint32_t target, float2 renderSize);
	/** @brief Draws the bounded scheduling diagnostics overlay. */
	void DrawDiagnosticsOverlay();
	/** @brief True while the developer has switched the diagnostics overlay on. */
	bool DiagnosticsOverlayVisible() const { return diagnostics.OverlayVisible(); }
	/** @brief Records progress through the existing post-processing chain. */
	void RecordStage(bool finishedPost);
	/** @brief Captures the scene immediately before the existing upscaler. */
	void CaptureBeforeUpscaling();
	/** @brief Captures the scene immediately after the existing upscaler. */
	void CaptureAfterUpscaling();

	/** @brief Snapshot of the current status, safe from any thread. */
	Status GetStatus() const;
	/** @brief Queues one retry for the next world frame; all the Retry action does. */
	void RequestRetry() { retryRequested = resetHistory = true; }
	/** @brief Queues one lossless DDS capture of every NR stage in the next NR frame. */
	void RequestCapture() { diagnostics.RequestCapture(); }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	NR::Diagnostics diagnostics;
	std::atomic_bool resetHistory = true, recreate = false, clearShaders = false, retryRequested = false;
	Status status;
	/** @brief Mirror of status.state so the per-frame paths can skip a publish without the lock. */
	std::atomic<Status::State> publishedState{ Status::State::kOff };
	/** @brief Engine frame of the last frame NR applied; reported live, unlike the status snapshot. */
	std::atomic<uint32_t> appliedFrame{ UINT32_MAX };
	mutable std::mutex statusMutex;
	/** @brief Publishes a status line plus the run state the panel and devbench report. */
	void PublishStatus(Status::State state, std::string text);
	/** @brief Republishes the render size and eye count after the pass resources are recreated. */
	void PublishResources();
	/** @brief Publishes a failure with the prefix the panel shows for a stopped pass. */
	void PublishFailure(const std::string& detail);
	/** @brief Latches a failure, tearing the runtime down first when the device was removed. */
	void LatchFailure();
	/** @brief Last member: it must be destroyed before impl's NGX teardown at process exit. */
	NR::TerminationSentinel terminationSentinel;
};
