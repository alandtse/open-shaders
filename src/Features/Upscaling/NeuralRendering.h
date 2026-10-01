#pragma once

#include "NeuralRendering/ActorRegion.h"
#include "NeuralRendering/CropCalibration.h"
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
		/** @brief Per-eye render width NR last created its pass resources for. */
		uint32_t width = 0;
		/** @brief Render height NR last created its pass resources for. */
		uint32_t height = 0;
		/** @brief Eye count: 1 on flat, 2 on VR. */
		uint32_t eyes = 0;
		/** @brief Engine frame of the last frame NR applied; UINT32_MAX before the first. */
		uint32_t lastAppliedFrame = UINT32_MAX;
		/** @brief Frames NR has applied since startup. */
		uint32_t appliedFrames = 0;
		/** @brief Per-eye NGX result code of the last applied frame. */
		std::array<uint32_t, 2> ngxResult{};
		/** @brief True while a failure is latched; the next successful frame clears it. */
		bool failed = false;
	};

	NeuralRendering();
	~NeuralRendering();
	/** @brief Installs the main-thread hook that tracks the actor NR scopes its evaluation to. */
	void InstallHooks();
	/** @brief Schedules recreation on the rendering thread. */
	void SetupResources();
	/** @brief Invalidates both temporal histories on loading or setting changes. */
	void ResetHistory();
	/** @brief Discards history when NR or the world is inactive, and mirrors the crop controls for the main thread. */
	void Reset(bool enabled, bool regionOfInterest, uint32_t cropFit, bool cropGroup);
	/**
	 * @brief Republishes the tracked actor's per-eye crop; runs on the main thread.
	 *        Crop pixels are NR's render pixels, not the screen's, and the hook measures nothing
	 *        until NR is active, so it reads no engine state before the device exists.
	 */
	void UpdateRegionOfInterest();
	/** @brief Drives one frame of the calibration sweep: forces the current crop and times NR. */
	void UpdateCalibration();
	/** @brief Invalidates the cached existing upscaling encoder. */
	void ClearShaderCache();
	/** @brief Draws Upscaling's NR tuning, retry controls, and runtime status. */
	void DrawSettings(bool& enabled, NR::Tuning& tuning);
	/** @brief Draws the runtime DLL's verdict and how to fix it, under Upscaling's DLL tables. */
	void DrawRuntimeDiagnostics() const;
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
	/** @brief Cached verdict for the runtime on disk: whether NR can start, and why not when it cannot. */
	NR::RuntimeAvailability GetRuntimeAvailability() const;
	/** @brief Snapshot of the tracked actor's crop, safe from any thread. */
	Util::Region::StereoRegion GetRegionOfInterest() const;
	/** @brief Snapshot of the tracked actor's projected box with no padding and no stabilising, safe from any thread. */
	Util::Region::StereoRegion GetActorBox() const;
	/** @brief Queues a sweep of centred crop sizes that measures how NR's GPU cost falls with crop area. */
	void RequestCalibration() { calibrationRequested.store(true, std::memory_order_relaxed); }
	/** @brief Snapshot of the latest calibration; running, done with its measurements, or failed with why. */
	NR::CropCalibration::Result GetCalibration() const;
	/** @brief Cumulative NR counters since startup, safe from any thread. */
	NR::Diagnostics::Counters GetDiagnosticCounters() const { return diagnostics.GetCounters(); }
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
	/** @brief Frames NR has applied since startup; survives a rebuild, unlike Impl's own state. */
	std::atomic<uint32_t> appliedFrames{ 0 };
	/** @brief Per-eye NGX result of the last applied frame; survives a rebuild. */
	std::array<std::atomic<uint32_t>, 2> lastNgxResult{};
	mutable std::mutex statusMutex;
	/** @brief True while the region-of-interest toggle is on and NR is enabled; the hook's off switch. */
	std::atomic_bool regionEnabled = false;
	/** @brief How the crop is fit to the projected bounds, mirrored from the tuning like regionEnabled. */
	std::atomic<uint32_t> regionFit{ NR::Tuning::kRegionFitPadded };
	/** @brief Whether the crop covers several actors, mirrored from the tuning like regionEnabled. */
	std::atomic_bool regionGroup = false;
	/** @brief The tracked actor's crop, written by the main thread and read by the rendering thread. */
	Util::Region::StereoRegion region;
	/**
	 * @brief The tracked actor's own projected box, written with region but never stabilised: it is the
	 *        raw pick, so a crop that flaps around the actor stays visible instead of being smoothed away.
	 */
	Util::Region::StereoRegion actorBox;
	/** @brief Holds the crop steady across jitter; advanced only on the main thread inside UpdateRegionOfInterest. */
	Util::Region::RegionStabilizer regionStabilizer{ NR::ActorRegion::kStabilizerPolicy };
	/**
	 * @brief Actor the crop followed on the previous frame; candidates score against it for stickiness.
	 *        Written and read only on the main thread inside UpdateRegionOfInterest, never by the
	 *        rendering thread, so it needs no lock.
	 */
	RE::ActorHandle trackedActor;
	mutable std::mutex regionMutex;
	/** @brief Set by the settings button or DevBench; the main-thread hook starts the sweep on its next frame. */
	std::atomic_bool calibrationRequested = false;
	/** @brief Main-thread sweep state; its published copy is guarded by regionMutex. */
	NR::CropCalibration calibration;
	/** @brief Captured GPU frame the sweep last sampled, so one capture is never counted twice. */
	uint32_t lastCalibrationGpuFrame = 0;
	NR::CropCalibration::Result calibrationResult;
	/** @brief Crop area share the last completed calibration found free, or zero; read by the main-thread tracker only. */
	float calibratedKneeFraction = 0.0f;
	/** @brief Draws the settings panel's crop preview: the NR-resolution scene with the per-eye crops. */
	void DrawRegionPreview();
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
