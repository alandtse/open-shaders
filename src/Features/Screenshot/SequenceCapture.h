#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>

/** Native lossless frame sequences with correlated stereo acquisition and bounded readback. */
class ScreenshotSequenceCapture
{
public:
	/** Creates an idle controller; resources and the encoding worker are lazy. */
	ScreenshotSequenceCapture();
	/** Stops scheduling and joins the encoding worker. */
	~ScreenshotSequenceCapture();
	/** Dispatches versioned sequence commands on the game thread. */
	nlohmann::json HandleRequest(const nlohmann::json& request, bool enabled, bool stillCapturePending);
	/** Queues a native reference PNG and reports completion to the capture provider. */
	nlohmann::json HandleReferenceRequest(const nlohmann::json& request, bool enabled, bool stillCapturePending,
		std::function<void(const nlohmann::json&)> completion);
	/** Advances nonblocking GPU readback on the render thread after Present. */
	void Tick(bool enabled);
	/** Cancels render-thread acquisition when the feature is disabled. */
	void Stop();
	/** Remains active until acquired frames and their manifest are finalized. */
	bool IsActive() const;

private:
	struct Impl;
	std::shared_ptr<Impl> impl;
};
