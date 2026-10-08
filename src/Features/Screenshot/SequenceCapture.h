#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <winrt/base.h>

/** Native lossless frame sequences with correlated stereo acquisition and bounded readback. */
class ScreenshotSequenceCapture
{
public:
	using DesktopSource = std::function<winrt::com_ptr<ID3D11Texture2D>()>;

	/** Creates an idle controller using the feature's presented-buffer resolver. */
	explicit ScreenshotSequenceCapture(DesktopSource desktopSource);
	/** Stops scheduling and bounds the wait for isolated encoding work to two seconds. */
	~ScreenshotSequenceCapture();
	/** Dispatches versioned sequence commands on the game thread. */
	nlohmann::json HandleRequest(const nlohmann::json& request, bool enabled, bool stillCapturePending);
#ifdef DEVBENCH_BRIDGE_ENABLED
	/** Queues a native reference PNG and reports completion to the capture provider. */
	nlohmann::json HandleReferenceRequest(const nlohmann::json& request, bool enabled, bool stillCapturePending,
		std::function<void(const nlohmann::json&)> completion);
#endif
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
