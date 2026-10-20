#include "Features/Screenshot/BurstAdapter.h"

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Features/Screenshot/Service.h"
#	include "Features/Screenshot/Storage.h"

#	include <fstream>

namespace OS::Capture::Burst
{
	namespace
	{
		constexpr auto kPollInterval = std::chrono::milliseconds(250);

		/** Hard-links a frame under its burst name, copying when the volume cannot link. */
		bool LinkFrame(const FrameFile& a_file)
		{
			std::error_code error;
			std::filesystem::create_hard_link(a_file.source, a_file.target, error);
			if (!error)
				return true;
			return std::filesystem::copy_file(a_file.source, a_file.target, std::filesystem::copy_options::none, error) && !error;
		}

		bool WriteManifest(const std::filesystem::path& a_path, const json& a_manifest)
		{
			try {
				std::error_code error;
				std::filesystem::create_directories(a_path.parent_path(), error);
				const auto text = a_manifest.dump(1);
				OS::CaptureStorage::CommittedFile::WriteAtomically(std::filesystem::path(a_path.wstring() + L".writing"), a_path, text.data(), text.size(), false);
				return true;
			} catch (const std::exception& failure) {
				logger::warn("Screenshot burst manifest failed: {}", failure.what());
				return false;
			}
		}
	}

	Adapter::~Adapter()
	{
		if (worker.joinable())
			worker.join();
	}

	bool Adapter::Start(Request a_request, bool a_vr, const Dispatch& a_dispatch, Notify a_progress, Notify a_finished, uint64_t& a_runId, std::string& a_error)
	{
		if (const auto problem = Validate(a_request); !problem.empty()) {
			a_error = problem;
			return false;
		}
		std::lock_guard lock(mutex);
		if (status.active || publishing) {
			a_error = "another burst is still running";
			return false;
		}
		const auto response = a_dispatch(SequenceStartCommand(a_request, a_vr));
		if (!response.value("ok", false)) {
			const auto& failure = response.contains("error") ? response["error"] : json::object();
			a_error = failure.value("message", std::string("the frame sequence could not start"));
			return false;
		}
		if (worker.joinable())
			worker.join();
		sequenceId = response.at("result").at("requestId").get<std::string>();
		if (a_request.manifestPath.empty())
			a_request.manifestPath = a_request.outputDir / std::format("{}_manifest.json", a_request.prefix);
		request = std::move(a_request);
		status = {};
		status.runId = nextRunId++;
		status.requestId = request.requestId;
		status.active = true;
		status.requested = request.count;
		progress = std::move(a_progress);
		finished = std::move(a_finished);
		publishResult.reset();
		nextPoll = {};
		a_runId = status.runId;
		return true;
	}

	void Adapter::Publish(json a_engineManifest, Request a_request)
	{
		Status result;
		result.requestId = a_request.requestId;
		result.requested = a_request.count;
		try {
			auto converted = Convert(a_engineManifest, a_request);
			result = converted.counts;
			result.requestId = a_request.requestId;
			result.requested = a_request.count;
			result.uiExcluded = converted.counts.uiExcluded;
			if (!a_engineManifest.value("error", std::string{}).empty())
				result.error = a_engineManifest["error"].get<std::string>();
			std::error_code error;
			std::filesystem::create_directories(a_request.outputDir, error);
			json linked = json::array();
			auto& manifestFrames = converted.manifest["frames"];
			uint32_t unlinked = 0;
			for (size_t index = 0; index < converted.files.size(); ++index) {
				if (LinkFrame(converted.files[index])) {
					linked.push_back(std::move(manifestFrames[index]));
				} else {
					++unlinked;
					++result.failed;
					--result.captured;
				}
			}
			manifestFrames = std::move(linked);
			if (unlinked && result.error.empty())
				result.error = std::format("{} frame(s) could not be written; an output file may already exist", unlinked);
			if (WriteManifest(a_request.manifestPath, converted.manifest))
				result.manifestPath = a_request.manifestPath;
			else if (result.error.empty())
				result.error = "the burst manifest could not be written";
		} catch (const std::exception& failure) {
			logger::warn("Screenshot burst publication failed: {}", failure.what());
			result.error = failure.what();
		} catch (...) {
			result.error = "burst publication failed";
		}
		std::lock_guard lock(mutex);
		publishResult = std::move(result);
	}

	void Adapter::Tick(const Dispatch& a_dispatch)
	{
		Notify notify;
		Status snapshot;
		bool progressed = false;
		Notify progressNotify;
		{
			std::lock_guard lock(mutex);
			if (publishResult) {
				auto result = std::move(*publishResult);
				publishResult.reset();
				result.runId = status.runId;
				result.finished = true;
				result.active = false;
				status = result;
				publishing = false;
				notify = std::move(finished);
				snapshot = status;
			} else if (status.active && !publishing && std::chrono::steady_clock::now() >= nextPoll) {
				nextPoll = std::chrono::steady_clock::now() + kPollInterval;
				try {
					const json poll = { { "contractMajor", 1 }, { "clientId", "devbench.capture.burst" },
						{ "commandId", ServiceFoundation::NewId() }, { "action", "request_get" }, { "requestId", sequenceId } };
					const auto response = a_dispatch(poll);
					if (!response.value("ok", false)) {
						const auto failure = response.value("error", json::object());
						if (failure.value("code", std::string{}) == "unknown_request")
							throw std::runtime_error(failure.value("message", std::string("the sequence receipt is no longer available")));
						return;
					}
					const auto& receipt = response.at("result");
					const auto written = receipt.value("written", 0u);
					const bool changed = written != status.captured;
					status.captured = written;
					status.dropped = receipt.value("dropped", 0u);
					status.failed = receipt.value("failed", 0u);
					if (changed && progress) {
						progressed = true;
						progressNotify = progress;
						snapshot = status;
					}
					if (IsTerminalState(receipt.value("state", std::string{}))) {
						json manifest = json::object();
						std::ifstream file(std::filesystem::u8path(receipt.value("directory", std::string{})) / "manifest.json", std::ios::binary);
						if (file) {
							manifest = json::parse(file, nullptr, false);
							if (manifest.is_discarded())
								manifest = json::object();
						}
						if (manifest.empty())
							manifest = { { "error", receipt.value("error", std::string("the frame sequence ended without a manifest")) } };
						else if (!receipt.value("error", std::string{}).empty())
							manifest["error"] = receipt["error"];
						publishing = true;
						if (worker.joinable())
							worker.join();
						worker = std::thread([this, manifest = std::move(manifest), burst = request]() mutable { Publish(std::move(manifest), std::move(burst)); });
					}
				} catch (const std::exception& failure) {
					logger::warn("Screenshot burst polling failed: {}", failure.what());
					Status failed;
					failed.requestId = request.requestId;
					failed.requested = request.count;
					failed.error = failure.what();
					publishing = true;
					publishResult = std::move(failed);
				}
			}
		}
		if (progressed)
			progressNotify(snapshot);
		else if (notify)
			notify(snapshot);
	}

	std::optional<Status> Adapter::GetStatus(uint64_t a_runId) const
	{
		std::lock_guard lock(mutex);
		if (a_runId == 0 || status.runId != a_runId)
			return std::nullopt;
		return status;
	}

	bool Adapter::IsActive() const
	{
		std::lock_guard lock(mutex);
		return status.active || publishing;
	}
}

#endif
