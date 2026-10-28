#pragma once

#include "Feature.h"
#include "Utils/Subrect.h"
#ifdef DEVBENCH_BRIDGE_ENABLED
#	include "Features/Screenshot/BurstAdapter.h"
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class ScreenshotSequenceCapture;

namespace DirectX
{
	class ScratchImage;
}

struct ScreenshotFeature : public Feature
{
	/** Leaves the native sequence controller idle until capture is requested. */
	ScreenshotFeature();
	/** @brief Stops the background screenshot worker thread on destruction. */
	virtual ~ScreenshotFeature();
	virtual std::string GetName() override { return "Screenshot"; }
	virtual std::string GetDisplayName() override { return T("feature.screenshot.name", "Screenshot"); }
	virtual std::string GetShortName() override { return "Screenshot"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }

	virtual bool SupportsVR() override { return true; }  // fork: VR screenshots (per-eye Subrect presets)

	/** @brief Returns true, indicating this feature's settings are always visible in the menu. */
	virtual bool IsInMenu() const override;

	/** @brief Draws the ImGui settings UI for screenshot path, format, crop, and hotkey configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& a_json) override;
	virtual void SaveSettings(json& a_json) override;
	/** @brief Resets transient state (no-op for this feature). */
	virtual void Reset() override;
	/** @brief Called after all features are loaded (no-op for this feature). */
	virtual void PostPostLoad() override;

	/** @brief How a requested capture ended, reported from the save worker thread. */
	struct CaptureResult
	{
		bool ok = false;
		uint32_t width = 0;
		uint32_t height = 0;
		/** @brief True only when the saved image is a source drawn without the HUD. */
		bool uiExcluded = false;
		/** @brief Engine frame whose image was copied on the render thread. */
		uint32_t renderFrame = 0;
	};

	/** @brief One capture to an explicit PNG path, of the whole source or a UV sub-region, with an optional completion callback. */
	struct CaptureRequest
	{
		std::filesystem::path outputPath;
		std::optional<Util::Subrect::UVRegion> region;
		/** @brief Use a HUD-free source when one exists (flat HDR only); otherwise the capture includes the HUD and reports it. */
		bool excludeUi = false;
		std::function<void(const CaptureResult&)> onComplete;
	};

	/**
	 * @brief Queues one capture for the next present. The file is a PNG at request.outputPath and gets
	 *        no crop preset, clipboard copy or HUD notice, so a tool can call it between user captures.
	 *        A user capture requested in the same frame is served on the following frame.
	 * @return False when another request is already waiting or a frame sequence is active.
	 */
	bool RequestCapture(CaptureRequest request);

	/** @brief Captures a screenshot from the current back buffer and enqueues it for async encoding and save. A request saves to its own path instead of the user's screenshot folder. */
	void Capture(std::optional<CaptureRequest> request = std::nullopt);
	/** @brief Checks for a pending capture request and executes Capture() if one is pending. Should be called before the wrapped buffers are cleared. */
	void ProcessCaptureRequest();
	/** Dispatches the explicit, versioned native frame-sequence API. */
	json HandleSequenceRequest(const json& request);
#ifdef DEVBENCH_BRIDGE_ENABLED
	/** Supplies DevBench's registered golden-reference capture provider. */
	json HandleReferenceCapture(const json& request, std::function<void(const json&)> completion);
	/**
	 * @brief Starts a DevBench `capture` burst on the native sequence engine; the frames land as <prefix>_NNNN.png in
	 *        request.outputDir with a manifest beside them. a_progress and a_finished run on the render thread.
	 * @return False with a reason in a_error when the burst cannot start.
	 */
	bool StartBurst(OS::Capture::Burst::Request request, OS::Capture::Burst::Adapter::Notify a_progress, OS::Capture::Burst::Adapter::Notify a_finished, uint64_t& a_runId, std::string& a_error);
	/** @return The latest burst's status when a_runId names it. */
	[[nodiscard]] std::optional<OS::Capture::Burst::Status> GetBurstStatus(uint64_t a_runId) const;
#endif
	/** Stops frame scheduling when the feature is disabled. */
	void OnRuntimeDisabled() override;
	bool applyCropToScreenshot = true;

	// Settings
	std::string screenshotPath = "Screenshots";
	// HDR PNG quantization (7-16); used when HDR Display captures the back buffer.
	unsigned int hdrPngBitDepth = 11;
	// SDR output (HDR captures always use PNG).
	bool sdrUsePng = true;
	// After save, put the file path on the clipboard (CF_HDROP).
	bool copyToClipboard = false;

	std::atomic<bool> captureRequested{ false };

private:
#ifdef DEVBENCH_BRIDGE_ENABLED
	OS::Capture::Burst::Adapter burstAdapter;
#endif
	const std::unique_ptr<ScreenshotSequenceCapture> sequenceCapture;
	int sequenceFrameCount;
	int sequenceIntervalFrames = 1;
	int sequenceStartDelayFrames = 0;
	bool uiSequencePending = false;
	std::string uiSequenceId;
	std::string uiSequenceMessage;
	std::chrono::steady_clock::time_point nextSequencePoll{};
	void DrawSequenceSettings();
	struct PendingScreenshot
	{
		winrt::com_ptr<ID3D11Texture2D> stagingTexture;
		/// Filled on the render thread once the GPU copy lands; null if the readback failed.
		std::shared_ptr<DirectX::ScratchImage> image;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t width = 0;
		uint32_t height = 0;
		std::filesystem::path outputPath;
		bool saveAsHdrPng = false;
		bool saveAsSdrPng = false;
		int hdrPngBitDepth = 11;
		bool copyToClipboard = false;
		bool silent = false;
		bool uiExcluded = false;
		uint32_t renderFrame = 0;
		std::function<void(const CaptureResult&)> onComplete;
	};

	std::mutex pendingRequestMutex;
	std::optional<CaptureRequest> pendingRequest;

	std::mutex screenshotQueueMutex;
	std::condition_variable screenshotQueueCV;
	std::queue<PendingScreenshot> screenshotQueue;
	std::thread screenshotWorker;
	bool screenshotWorkerRunning = false;
	std::atomic<uint32_t> screenshotsInFlight{ 0 };
	/// Render thread only: captures whose GPU copy has not been read back yet.
	std::vector<PendingScreenshot> copiesInFlight;
	Util::Subrect::Controller subrect;

	// SRV-readable copy used when the capture source's own SRV can't be sampled
	// directly (kFRAMEBUFFER on flat aliases the swap-chain backbuffer).
	winrt::com_ptr<ID3D11Texture2D> previewCacheTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> previewCacheSRV;

	void EnsureWorkerThread();
	void StopWorkerThread();
	void EnqueueScreenshot(PendingScreenshot&& screenshot);
	/** @brief Reads back every finished GPU copy without waiting and hands it to the worker; render thread only. */
	void EnqueueFinishedCopies();
	void ScreenshotWorkerLoop();
	void EnsurePreviewCache(ID3D11Texture2D* sourceTexture);
	static void ShowInGameNotification(std::string message);
};
