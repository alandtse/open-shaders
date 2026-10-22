#pragma once

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Features/Screenshot/SequencePolicy.h"

#	include <chrono>
#	include <cstdint>
#	include <filesystem>
#	include <format>
#	include <functional>
#	include <mutex>
#	include <nlohmann/json.hpp>
#	include <optional>
#	include <string>
#	include <thread>
#	include <vector>

namespace OS::Capture::Burst
{
	using json = nlohmann::json;

	inline constexpr uint32_t MaximumCount = 600;
	inline constexpr uint32_t MaximumEveryNFrames = 600;

	/** A DevBench `capture` burst: count PNGs, one every everyNFrames engine frames, named <prefix>_NNNN.png. */
	struct Request
	{
		std::string requestId;
		std::string prefix;
		std::filesystem::path outputDir;
		std::filesystem::path manifestPath;
		uint32_t count = 0;
		uint32_t everyNFrames = 1;
		bool excludeUi = true;
	};

	/** Progress of the latest burst; manifestPath is set only after the manifest was written. */
	struct Status
	{
		uint64_t runId = 0;
		std::string requestId;
		bool active = false;
		bool finished = false;
		uint32_t requested = 0;
		uint32_t captured = 0;
		uint32_t dropped = 0;
		uint32_t failed = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		bool uiExcluded = false;
		std::string error;
		std::filesystem::path manifestPath;
	};

	/** @return An empty string when the request is usable, otherwise why it is not. */
	inline std::string Validate(const Request& a_request)
	{
		if (a_request.requestId.empty() || a_request.requestId.size() > 128)
			return "burst needs a requestId of 1 to 128 characters";
		if (a_request.prefix.empty() || a_request.prefix.find_first_of("/\\:") != std::string::npos)
			return "prefix must be a non-empty file stem without path separators";
		if (!a_request.outputDir.is_absolute())
			return "outputDir must be an absolute path";
		if (a_request.count < 1 || a_request.count > MaximumCount)
			return std::format("count must be 1 to {}", MaximumCount);
		if (a_request.everyNFrames < 1 || a_request.everyNFrames > MaximumEveryNFrames)
			return std::format("everyNFrames must be 1 to {}", MaximumEveryNFrames);
		return {};
	}

	/** Maps the burst onto the native sequence engine: whole frames, PNG, one slot every everyNFrames engine frames. */
	inline json SequenceStartCommand(const Request& a_request, bool a_vr)
	{
		return { { "contractMajor", 1 }, { "clientId", "devbench.capture.burst" }, { "commandId", a_request.requestId },
			{ "action", "sequence_start" },
			{ "sequence", { { "frameCount", a_request.count }, { "useSettings", false },
							  { "schedule", { { "basis", "game_frames" }, { "intervalFrames", a_request.everyNFrames }, { "startDelayFrames", 0 } } },
							  { "capture", NativeCaptureDescriptor(a_vr, a_request.outputDir, true) },
							  { "packaging", { { "frameManifest", true } } } } } };
	}

	/** Name of the n-th burst image, as DevBench's progress counter expects it. */
	inline std::string FrameName(const std::string& a_prefix, uint32_t a_index)
	{
		return std::format("{}_{:04}.png", a_prefix, a_index);
	}

	struct FrameFile
	{
		std::filesystem::path source;
		std::filesystem::path target;
	};

	struct Converted
	{
		json manifest;
		std::vector<FrameFile> files;
		Status counts;
	};

	/**
	 * @brief Turns the engine's finished manifest into the burst layout: renamed frame links and the
	 *        {requestId,width,height,format,uiExcluded,everyNFrames,frames,dropped} manifest.
	 *        wallMs is measured from the first observed frame; gameTimeMs is the same clock because the
	 *        engine reports no per-frame delta.
	 */
	inline Converted Convert(const json& a_engineManifest, const Request& a_request)
	{
		Converted converted;
		json frames = json::array();
		json dropped = json::array();
		uint64_t firstUs = 0;
		bool haveFirst = false;
		for (const auto& entry : a_engineManifest.value("frames", json::array())) {
			const auto ordinal = entry.value("ordinal", 0u);
			if (ordinal < 1)
				continue;
			const uint32_t index = ordinal - 1;
			const auto state = entry.value("state", std::string{});
			if (state == "dropped") {
				dropped.push_back({ { "index", index }, { "gameFrame", entry.value("engineFrame", uint64_t{ 0 }) } });
				++converted.counts.dropped;
				continue;
			}
			if (state != "written" || !entry.contains("artifact") || !entry["artifact"].is_object()) {
				++converted.counts.failed;
				continue;
			}
			const auto& artifact = entry["artifact"];
			const auto micros = entry.value("monotonicTimestampUs", uint64_t{ 0 });
			if (!haveFirst) {
				firstUs = micros;
				haveFirst = true;
			}
			const double elapsedMs = micros >= firstUs ? double(micros - firstUs) / 1000.0 : 0.0;
			const auto name = FrameName(a_request.prefix, index);
			frames.push_back({ { "index", index }, { "file", name }, { "gameFrame", entry.value("engineFrame", uint64_t{ 0 }) },
				{ "gameTimeMs", elapsedMs }, { "wallMs", elapsedMs } });
			converted.files.push_back({ std::filesystem::u8path(artifact.value("path", std::string{})), a_request.outputDir / name });
			converted.counts.width = artifact.value("width", 0u);
			converted.counts.height = artifact.value("height", 0u);
			if (converted.counts.captured == 0)
				converted.counts.uiExcluded = artifact.contains("planes") && artifact["planes"].is_array() && !artifact["planes"].empty() &&
				                              std::ranges::all_of(artifact["planes"], [](const json& a_plane) { return a_plane.value("uiExcluded", false); });
			++converted.counts.captured;
		}
		converted.manifest = { { "requestId", a_request.requestId }, { "width", converted.counts.width }, { "height", converted.counts.height },
			{ "format", "png" }, { "uiExcluded", converted.counts.uiExcluded }, { "everyNFrames", a_request.everyNFrames },
			{ "frames", std::move(frames) }, { "dropped", std::move(dropped) } };
		return converted;
	}

	/** One burst at a time, driven by the feature's render-thread tick; file work runs on a worker. */
	class Adapter
	{
	public:
		using Dispatch = std::function<json(const json&)>;
		using Notify = std::function<void(const Status&)>;

		/** Joins the finalization worker. */
		~Adapter();
		/** Starts the sequence; a_error says why a burst could not start. */
		bool Start(Request a_request, bool a_vr, const Dispatch& a_dispatch, Notify a_progress, Notify a_finished, uint64_t& a_runId, std::string& a_error);
		/** Polls the sequence receipt and publishes the burst when it finished; call once per frame on the render thread. */
		void Tick(const Dispatch& a_dispatch);
		/** @return The latest burst's status when a_runId names it. */
		std::optional<Status> GetStatus(uint64_t a_runId) const;
		/** @return Whether a burst is waiting for its sequence or its publication. */
		bool IsActive() const;

	private:
		mutable std::mutex mutex;
		Request request;
		Status status;
		Notify progress;
		Notify finished;
		std::string sequenceId;
		uint64_t nextRunId = 1;
		std::chrono::steady_clock::time_point nextPoll{};
		std::thread worker;
		bool publishing = false;
		std::optional<Status> publishResult;

		void Publish(json a_engineManifest, Request a_request);
	};
}

#endif
