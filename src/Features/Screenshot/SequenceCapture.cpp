#include "Features/Screenshot/SequenceCapture.h"

#include "Features/Screenshot/SequenceImage.h"
#include "Features/Screenshot/SequencePolicy.h"
#include "Features/Screenshot/Service.h"
#include "Features/Screenshot/Storage.h"
#include "Features/Screenshot/WorkerThread.h"
#ifdef DEVBENCH_BRIDGE_ENABLED
#	include "Features/Screenshot/ReferenceCompletion.h"
#endif
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/StringUtils.h"
#include <DirectXTex.h>
#include <RE/B/BSOpenVR.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
	using json = nlohmann::json;
	using Service = OS::Capture::ServiceFoundation;
	using OS::CaptureStorage::CommittedFile;
	using OS::CaptureStorage::DirectoryLease;
	constexpr std::size_t kRetainedSequences = 8;

	uint64_t FrameNumber()
	{
		return globals::state ? globals::state->frameCountAtomic.load(std::memory_order_acquire) : 0;
	}

	uint64_t TimestampUs()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	bool SupportedFormat(DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
		       format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	}

	void CheckResult(HRESULT result)
	{
		if (FAILED(result))
			throw std::runtime_error(std::format("capture failed with HRESULT {:#x}", static_cast<uint32_t>(result)));
	}
}

struct ScreenshotSequenceCapture::Impl : std::enable_shared_from_this<Impl>
{
	struct Plane
	{
		winrt::com_ptr<ID3D11Texture2D> staging;
		OS::Capture::PixelBounds bounds{};
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		json metadata;
		std::vector<uint8_t> pixels;
	};

	struct Session
	{
		std::string id;
		OS::Capture::SequenceSchedule schedule;
		std::shared_ptr<DirectoryLease> directory;
		std::filesystem::path directoryPath;
		json requested;
		json frames = json::array();
		json manifest = nullptr;
#ifdef DEVBENCH_BRIDGE_ENABLED
		std::filesystem::path referencePath;
		std::string referenceId;
		std::function<void(const json&)> referenceCompletion;
#endif
		std::string state = "preparing";
		std::string error;
#ifdef DEVBENCH_BRIDGE_ENABLED
		ScreenshotBurst::Continuity continuity;
		json sourceIdentity = nullptr;
#endif
		uint32_t outstanding = 0;
		uint32_t consecutiveSkips = 0;
		bool preparing = true;
		bool started = false;
		bool stopping = false;
		bool finalizing = false;
		bool stoppedByUser = false;
		uint64_t pausedAtFrame = 0;
		uint64_t firstCycle = 0;
		uint64_t lastEngineFrame = 0;
	};

	struct Frame
	{
		std::shared_ptr<Session> session;
		std::array<Plane, 2> eyes;
		uint64_t engineFrame = 0;
		uint64_t cycle = 0;
		uint64_t bytes = 0;
		uint32_t ordinal = 0;
		uint8_t eyeMask = 0;
		uint8_t requiredMask = 3;
		winrt::com_ptr<ID3D11Device> device;
	};

	explicit Impl(DesktopSource source) : desktopSource(std::move(source)) {}

	DesktopSource desktopSource;
	Service service{ { "openshaders.screenshot", 1, 0, 1 } };
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<std::shared_ptr<Session>> sessions;
	std::shared_ptr<Session> active;
	std::optional<Frame> acquiring;
	std::deque<Frame> readbacks;
	std::deque<Frame> encodes;
	std::unique_ptr<OS::Capture::WorkerThread<Impl>> worker;
	uint64_t reservedBytes = 0;
	uint64_t compositorCycle = 0;
	uint64_t scheduledCycle = 0;
	std::atomic<DWORD> renderThread{ 0 };
	std::atomic<bool> capturing{ false };
	bool paused = false;
	bool stopRequested = false;
	bool exited = false;
	bool hooksInstalled = false;
	inline static std::mutex hookMutex;
	inline static std::weak_ptr<Impl> hookOwner;
	inline static bool installed = false;

	struct WaitHook
	{
		static vr::EVRCompositorError thunk(vr::IVRCompositor* self, vr::TrackedDevicePose_t* render, uint32_t renderCount, vr::TrackedDevicePose_t* game, uint32_t gameCount)
		{
			const auto result = func(self, render, renderCount, game, gameCount);
			if (result == vr::VRCompositorError_None) {
				std::shared_ptr<Impl> owner;
				{
					std::lock_guard lock(hookMutex);
					owner = hookOwner.lock();
				}
				if (owner) {
					try {
						owner->BeginCycle();
					} catch (const std::exception& e) {
						owner->Abort(e.what());
					} catch (...) {
						owner->Abort("unknown_compositor_cycle_error");
					}
				}
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SubmitHook
	{
		static vr::EVRCompositorError thunk(vr::IVRCompositor* self, vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags)
		{
			const auto result = func(self, eye, texture, bounds, flags);
			if (result == vr::VRCompositorError_None) {
				std::shared_ptr<Impl> owner;
				{
					std::lock_guard lock(hookMutex);
					owner = hookOwner.lock();
				}
				if (owner) {
					try {
						owner->Observe(eye, texture, bounds, flags);
					} catch (const std::exception& e) {
						owner->Abort(e.what());
					} catch (...) {
						owner->Abort("unknown_eye_submission_error");
					}
				}
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Shutdown()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		{
			std::lock_guard lock(mutex);
			stopRequested = true;
			desktopSource = {};
			capturing.store(false, std::memory_order_release);
#ifdef DEVBENCH_BRIDGE_ENABLED
			for (const auto& session : sessions)
				session->referenceCompletion = {};
#endif
		}
		Abort("shutdown");
		condition.notify_all();
		if (worker && !worker->ShutdownUntil(deadline))
			logger::warn("Screenshot sequence shutdown deadline reached; isolated publication is still draining");
		worker.reset();
	}

	static void WorkerEntry(std::shared_ptr<Impl> state)
	{
		const SKSE::stl::scope_exit finished([&] {
			{
				std::lock_guard lock(state->mutex);
				state->exited = true;
			}
			state->condition.notify_all();
		});
		state->Worker();
	}

	bool InstallHooks()
	{
		std::lock_guard lock(hookMutex);
		if (!installed) {
			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor)
				return false;
			auto* table = *reinterpret_cast<std::uintptr_t**>(compositor);
			WaitHook::func = table[2];
			SubmitHook::func = table[5];
			LONG result = DetourTransactionBegin();
			if (result != NO_ERROR)
				return false;
			result = DetourUpdateThread(GetCurrentThread());
			if (result == NO_ERROR)
				result = DetourAttach(reinterpret_cast<PVOID*>(&WaitHook::func), reinterpret_cast<PVOID>(WaitHook::thunk));
			if (result == NO_ERROR)
				result = DetourAttach(reinterpret_cast<PVOID*>(&SubmitHook::func), reinterpret_cast<PVOID>(SubmitHook::thunk));
			if (result != NO_ERROR) {
				DetourTransactionAbort();
				return false;
			}
			if (DetourTransactionCommit() != NO_ERROR)
				return false;
			installed = true;
		}
		hookOwner = shared_from_this();
		return true;
	}

	json ReceiptLocked(const Session& session) const
	{
		uint32_t written = 0, dropped = 0, failed = 0;
		for (const auto& frame : session.frames) {
			written += frame.at("state") == "written";
			dropped += frame.at("state") == "dropped";
			failed += frame.at("state") == "failed";
		}
		auto receipt = json{ { "requestId", session.id }, { "state", session.state },
			{ "scheduled", session.frames.size() }, { "requested", session.schedule.plan.frameCount },
			{ "written", written }, { "dropped", dropped }, { "failed", failed },
			{ "inFlight", session.outstanding }, { "directory", Util::PathToUtf8(session.directoryPath) },
			{ "manifest", session.manifest }, { "error", session.error } };
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (session.schedule.plan.burst) {
			receipt["continuity"] = session.continuity.Receipt(session.schedule.plan.frameCount, written == session.schedule.plan.frameCount);
			receipt["bufferedPayloadBytes"] = session.schedule.plan.burst.payloadBytes;
		}
		if (!session.referencePath.empty()) {
			receipt["reference"] = { { "requestId", session.referenceId }, { "path",
										Util::PathToUtf8(session.referencePath) } };
			if (session.frames.size() == 1 && session.frames[0].at("state") == "written")
				receipt["reference"]["artifact"] = session.frames[0].at("artifact");
		}
#endif
		return receipt;
	}

	json Lookup(std::string_view id)
	{
		std::lock_guard lock(mutex);
		for (const auto& session : sessions) {
			if (session->id == id)
				return ReceiptLocked(*session);
		}
		return nullptr;
	}

#ifdef DEVBENCH_BRIDGE_ENABLED
	json Start(const json& request, bool enabled, bool stillCapturePending, std::function<void(const json&)> completion = {})
#else
	json Start(const json& request, bool enabled, bool stillCapturePending)
#endif
	{
		if (!enabled)
			return service.MakeError(request, "feature_disabled", "Screenshot is not loaded");
		if (stillCapturePending)
			return service.MakeError(request, "busy", "a still screenshot is pending or being saved", "dispatch", true);
		const auto plan = OS::Capture::ParsePlan(request.at("sequence"), globals::game::isVR);
		const auto directory = std::filesystem::u8path(plan.directory);
		if (!directory.is_absolute())
			throw std::invalid_argument("destination.directory must be absolute");
		std::lock_guard lock(mutex);
		if (active || stopRequested)
			return service.MakeError(request, "busy", "a sequence is recording or draining", "dispatch", true);
		if (globals::game::isVR && !hooksInstalled) {
			hooksInstalled = InstallHooks();
			if (!hooksInstalled)
				return service.MakeError(request, "source_unavailable", "OpenVR capture hooks could not be installed");
		}
		auto session = std::make_shared<Session>();
		session->id = Service::NewId();
		session->schedule.plan = plan;
		session->requested = request.at("sequence");
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (completion) {
			session->referencePath = std::filesystem::u8path(request.at("reference").at("outputPath").get<std::string>());
			session->referenceId = request.at("reference").at("requestId").get<std::string>();
			session->referenceCompletion = std::move(completion);
		}
#endif
		if (!worker)
			worker = std::make_unique<OS::Capture::WorkerThread<Impl>>(shared_from_this(), &WorkerEntry);
		while (sessions.size() >= kRetainedSequences)
			sessions.pop_front();
		sessions.push_back(session);
		active = session;
		capturing.store(true, std::memory_order_release);
		condition.notify_all();
		auto response = service.MakeEnvelope(request, true);
		response["result"] = ReceiptLocked(*session);
		return response;
	}

	void Prepare(const std::shared_ptr<Session>& session)
	{
		std::shared_ptr<DirectoryLease> directory;
		std::filesystem::path directoryPath;
#ifdef DEVBENCH_BRIDGE_ENABLED
		auto referencePath = session->referencePath;
#endif
		std::string error;
		try {
			directory = DirectoryLease::CreateExclusive(std::filesystem::u8path(session->schedule.plan.directory), session->id);
			directoryPath = directory->Path();
#ifdef DEVBENCH_BRIDGE_ENABLED
			if (!referencePath.empty()) {
				referencePath = directoryPath.parent_path() / referencePath.filename();
				directory->VerifyDestinationChild(referencePath);
				if (std::filesystem::exists(referencePath))
					throw std::invalid_argument("reference output already exists");
			}
#endif
		} catch (const std::exception& failure) {
			error = failure.what();
		}
		std::unique_lock lock(mutex);
		session->preparing = false;
		const bool failed = !error.empty();
		if (failed) {
			session->error = std::move(error);
			session->state = "failed";
			if (active == session) {
				active.reset();
				capturing.store(false, std::memory_order_release);
			}
		} else {
			session->directory = std::move(directory);
			session->directoryPath = std::move(directoryPath);
#ifdef DEVBENCH_BRIDGE_ENABLED
			session->referencePath = std::move(referencePath);
#endif
			session->firstCycle = compositorCycle + 1;
			session->state = session->stopping ? "draining" : "recording";
		}
		lock.unlock();
		directory.reset();
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (failed)
			CompleteReference(session);
#endif
		condition.notify_all();
	}

	json Dispatch(const json& request, bool enabled, bool stillCapturePending)
	{
		const auto action = request.at("action").get<std::string>();
		if (action == "sequence_start") {
			OS::Capture::CheckFields(request, { "contractMajor", "clientId", "commandId", "action", "sequence" });
			return Start(request, enabled, stillCapturePending);
		}
		if (action == "request_get" || action == "sequence_stop")
			OS::Capture::CheckFields(request, { "contractMajor", "clientId", "commandId", "action", "requestId" });
		else
			OS::Capture::CheckFields(request, { "contractMajor", "clientId", "commandId", "action" });
		auto response = service.MakeEnvelope(request, true);
		if (action == "capabilities") {
			response["result"] = {
				{ "actions", { "capabilities", "status", "sequence_start", "sequence_stop", "request_get" } },
				{ "sources", { globals::game::isVR ? "hmd_submission" : "desktop_mirror" } },
				{ "views", { globals::game::isVR ? "side_by_side" : "source_native" } },
				{ "formats", { "bmp", "png" } }, { "colourContracts", { "sdr_srgb" } },
				{ "scheduleBases", { "game_frames" } }, { "maximumFrames", OS::Capture::MaximumFrames },
				{ "maximumSpanFrames", OS::Capture::MaximumSpanFrames },
				{ "maximumQueuedBytes", OS::Capture::MaximumQueuedBytes },
				{ "maximumQueuedFrames", OS::Capture::MaximumQueuedFrames },
				{ "previewVideo", false }, { "cropping", false }, { "resizing", false }
			};
#ifdef DEVBENCH_BRIDGE_ENABLED
			response["result"]["burst"] = { { "maximumFrames", ScreenshotBurst::MaximumFrames }, { "maximumBytes", ScreenshotBurst::MaximumBytes },
				{ "maximumRegions", ScreenshotBurst::MaximumRegions }, { "nativeRegionAtlas", true }, { "deferredEncoding",
					true } };
#endif
		} else if (action == "status") {
			std::lock_guard lock(mutex);
			response["result"] = { { "enabled", enabled }, { "reservedBytes", reservedBytes },
				{ "active", active ? ReceiptLocked(*active) : json(nullptr) }, { "retained", json::array() } };
			for (const auto& session : sessions)
				response["result"]["retained"].push_back(ReceiptLocked(*session));
		} else if (action == "request_get") {
			response["result"] = Lookup(request.at("requestId").get<std::string>());
			if (response["result"].is_null())
				return service.MakeError(request, "unknown_request", "sequence receipt is not retained");
		} else if (action == "sequence_stop") {
			std::lock_guard lock(mutex);
			if (!active || active->id != request.at("requestId").get<std::string>())
				return service.MakeError(request, "unknown_request", "no matching active sequence");
			StopLocked("");
			response["result"] = ReceiptLocked(*active);
		} else {
			return service.MakeError(request, "unsupported_action", "consult capabilities for supported actions");
		}
		return response;
	}

	void StopLocked(std::string_view reason)
	{
		if (!active)
			return;
		if (active->finalizing)
			return;
		if (!active->stopping) {
			active->stoppedByUser = reason.empty();
			if (!reason.empty())
				active->error = reason;
		}
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (active->schedule.plan.burst && !reason.empty())
			active->continuity.Fail(reason);
#endif
		active->stopping = true;
		active->state = "draining";
		if (acquiring) {
			FinishFrameLocked(*acquiring, "dropped", "incomplete_stereo_pair");
			acquiring.reset();
		}
		condition.notify_all();
	}

	void Abort(std::string_view reason) noexcept
	{
		try {
			std::lock_guard lock(mutex);
			StopLocked(reason);
			while (!readbacks.empty()) {
				FinishFrameLocked(readbacks.front(), "failed", reason);
				readbacks.pop_front();
			}
		} catch (...) {
			logger::error("Screenshot sequence could not preserve its failure receipt");
		}
	}

	void FinishFrameLocked(Frame& frame, std::string_view state, std::string_view reason)
	{
		auto& session = *frame.session;
		auto& entry = session.frames.at(frame.ordinal - 1);
		entry["state"] = state;
		entry["reason"] = reason;
		reservedBytes -= frame.bytes;
		--session.outstanding;
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (session.schedule.plan.burst && state != "written") {
			session.continuity.Fail(reason);
			session.error = reason;
			session.stopping = true;
		}
#endif
		if ((state == "dropped" && ++session.consecutiveSkips >= session.schedule.plan.maximumConsecutiveSkips) ||
			(state == "failed" && session.schedule.plan.stopOnFailure)) {
			session.error = reason;
			session.stopping = true;
		}
		if (session.schedule.Finished() || session.stopping)
			session.state = "draining";
		condition.notify_all();
	}

	std::optional<Frame> ScheduleLocked(uint64_t engineFrame)
	{
		if (stopRequested || !active || active->preparing || active->stopping || active->schedule.Finished() || paused)
			return std::nullopt;
		auto& schedule = active->schedule;
		if (!active->started) {
			schedule.nextFrame = engineFrame + schedule.plan.startDelayFrames;
			active->started = true;
		}
		while (schedule.Due(engineFrame)) {
			Frame frame;
			frame.session = active;
			frame.ordinal = schedule.nextOrdinal;
			frame.engineFrame = engineFrame;
			frame.cycle = compositorCycle;
			frame.requiredMask = globals::game::isVR ? 3 : 1;
			active->frames.push_back({ { "ordinal", frame.ordinal }, { "scheduledEngineFrame", schedule.nextFrame },
				{ "engineFrame", engineFrame }, { "compositorCycle", globals::game::isVR ? json(compositorCycle) : json(nullptr) },
				{ "monotonicTimestampUs", TimestampUs() }, { "utcTimestamp", Service::TimestampUtc() },
				{ "state", "acquiring" } });
			++active->outstanding;
			const auto missed = schedule.nextFrame < engineFrame;
			schedule.Advance();
			if (missed || (!OS::Capture::IsBurst(schedule.plan) && active->outstanding > OS::Capture::MaximumQueuedFrames)) {
				FinishFrameLocked(frame, "dropped", missed ? "missed_cadence" : "encoder_backpressure");
				if (active->stopping)
					break;
				continue;
			}
			return frame;
		}
		return std::nullopt;
	}

	void BeginCycle()
	{
		if (!capturing.load(std::memory_order_acquire))
			return;
		std::lock_guard lock(mutex);
		++compositorCycle;
		if (acquiring) {
			FinishFrameLocked(*acquiring, "dropped", "incomplete_stereo_pair");
			acquiring.reset();
		}
	}

	bool StagePlane(Frame& frame, uint32_t eye, ID3D11Texture2D* texture, const std::array<float, 4>& bounds, vr::EColorSpace colorSpace)
	{
		if (GetCurrentThreadId() != renderThread.load(std::memory_order_acquire))
			throw std::runtime_error("capture callback is not on the observed render thread");
		D3D11_TEXTURE2D_DESC source{};
		texture->GetDesc(&source);
		const auto region = OS::Capture::ResolveBounds(source.Width, source.Height, bounds);
		if (!region || !SupportedFormat(source.Format) || source.SampleDesc.Count != 1 || source.ArraySize != 1 ||
			(colorSpace != vr::ColorSpace_Auto && colorSpace != vr::ColorSpace_Gamma))
			throw std::runtime_error("source must be a single-sample SDR RGBA8/BGRA8 texture with valid eye bounds");
#ifdef DEVBENCH_BRIDGE_ENABLED
		const auto& burst = frame.session->schedule.plan.burst;
		const auto copies = burst ? burst.Copies(region->width, region->height, region->flipX, region->flipY) : std::vector<ScreenshotBurst::Copy>{};
		const auto width = burst ? burst.width : region->width;
		const auto height = burst ? burst.height : region->height;
		const auto maximumBytes = burst ? burst.maximumBytes : OS::Capture::MaximumQueuedBytes;
#else
		const auto width = region->width;
		const auto height = region->height;
		const auto maximumBytes = OS::Capture::MaximumQueuedBytes;
#endif
		const auto bytes = uint64_t(width) * height * 4;
		if (bytes > OS::Capture::MaximumFrameBytes || frame.bytes > OS::Capture::MaximumFrameBytes - bytes || bytes > maximumBytes || reservedBytes > maximumBytes - bytes)
			return false;
		winrt::com_ptr<ID3D11Device> device;
		texture->GetDevice(device.put());
		if (frame.device && frame.device != device)
			throw std::runtime_error("stereo eyes belong to different D3D devices");
		winrt::com_ptr<ID3D11DeviceContext> context;
		device->GetImmediateContext(context.put());
		if (context.get() != globals::d3d::context)
			throw std::runtime_error("capture texture is not owned by the active rendering context");
		CS_GPU_PASS("Screenshot::SequenceCapture");
		frame.device = device;
		auto& plane = frame.eyes[eye];
		plane.bounds = *region;
		plane.bounds.width = width;
		plane.bounds.height = height;
		plane.format = source.Format;
		plane.metadata = { { "eye", frame.requiredMask == 3 ? (eye == 0 ? "left" : "right") : "source" }, { "sourceWidth", source.Width },
			{ "sourceHeight", source.Height }, { "width", region->width }, { "height", region->height },
			{ "bounds", bounds }, { "flipX", region->flipX }, { "flipY", region->flipY },
			{ "format", source.Format }, { "colourSpace", colorSpace }, { "monotonicTimestampUs", TimestampUs() } };
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (burst) {
			plane.metadata["regions"] = frame.session->requested.at("burst").at("regions");
			plane.metadata["atlasWidth"] = width;
			plane.metadata["atlasHeight"] = height;
		}
#endif
		source.Width = width;
		source.Height = height;
		source.MipLevels = 1;
		source.Usage = D3D11_USAGE_STAGING;
		source.BindFlags = 0;
		source.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		source.MiscFlags = 0;
		CheckResult(device->CreateTexture2D(&source, nullptr, plane.staging.put()));
		Util::SetResourceName(plane.staging.get(), "Screenshot::SequenceReadback");
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (burst) {
			for (const auto& copy : copies) {
				D3D11_BOX box{ region->x + copy.x, region->y + copy.y, 0,
					region->x + copy.x + copy.width, region->y + copy.y + copy.height, 1 };
				context->CopySubresourceRegion(plane.staging.get(), 0, 0, copy.destinationY, 0, texture, 0, &box);
			}
		} else
#endif
		{
			D3D11_BOX box{ region->x, region->y, 0, region->x + region->width, region->y + region->height, 1 };
			context->CopySubresourceRegion(plane.staging.get(), 0, 0, 0, 0, texture, 0, &box);
		}
		frame.bytes += bytes;
		reservedBytes += bytes;
		frame.eyeMask |= uint8_t(1u << eye);
		return true;
	}

	void Observe(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags)
	{
		if (!capturing.load(std::memory_order_acquire) || renderThread.load(std::memory_order_acquire) == 0)
			return;
		std::lock_guard lock(mutex);
		UpdateClockLocked();
		if (active && compositorCycle >= active->firstCycle && scheduledCycle != compositorCycle) {
			scheduledCycle = compositorCycle;
			acquiring = ScheduleLocked(FrameNumber());
		}
		if (!acquiring || !texture || (eye != vr::Eye_Left && eye != vr::Eye_Right))
			return;
		auto& frame = *acquiring;
		if (texture->eType != vr::TextureType_DirectX || !texture->handle || !OS::Capture::CanAcceptEye(eye, frame.eyeMask, FrameNumber(), frame.engineFrame)) {
			FinishFrameLocked(frame, "failed", "invalid_or_mismatched_eye_submission");
			acquiring.reset();
			return;
		}
		winrt::com_ptr<ID3D11Texture2D> source;
		CheckResult(static_cast<IUnknown*>(texture->handle)->QueryInterface(__uuidof(ID3D11Texture2D), source.put_void()));
		const std::array<float, 4> region = bounds ? std::array{ bounds->uMin, bounds->vMin, bounds->uMax, bounds->vMax } : std::array{ 0.f, 0.f, 1.f, 1.f };
		if (!StagePlane(frame, eye, source.get(), region, texture->eColorSpace)) {
			FinishFrameLocked(frame, "dropped", "readback_memory_limit");
			acquiring.reset();
			return;
		}
		frame.eyes[eye].metadata["submitFlags"] = static_cast<uint32_t>(flags);
		if (frame.eyeMask == frame.requiredMask) {
			if (frame.eyes[0].format != frame.eyes[1].format ||
				frame.eyes[0].metadata.at("colourSpace") != frame.eyes[1].metadata.at("colourSpace") ||
				frame.eyes[0].bounds.width != frame.eyes[1].bounds.width || frame.eyes[0].bounds.height != frame.eyes[1].bounds.height) {
				FinishFrameLocked(frame, "failed", "incompatible_stereo_pair");
				acquiring.reset();
				return;
			}
			frame.session->consecutiveSkips = 0;
			AcquiredLocked(std::move(frame));
			acquiring.reset();
		}
	}

	void AcquiredLocked(Frame&& frame)
	{
#ifdef DEVBENCH_BRIDGE_ENABLED
		auto& session = *frame.session;
		if (session.schedule.plan.burst) {
			json identity = json::array();
			for (uint32_t eye = 0; eye < (frame.requiredMask == 3 ? 2u : 1u); ++eye) {
				auto plane = frame.eyes[eye].metadata;
				plane.erase("monotonicTimestampUs");
				identity.push_back(std::move(plane));
			}
			if (session.sourceIdentity.is_null())
				session.sourceIdentity = identity;
			else if (session.sourceIdentity != identity)
				session.continuity.Fail("source_changed_during_burst");
			if (!session.continuity.Observe(frame.engineFrame, frame.requiredMask == 3 ? std::optional(frame.cycle) : std::nullopt)) {
				session.error = session.continuity.failure;
				session.stopping = true;
			}
		}
#endif
		readbacks.push_back(std::move(frame));
	}

	void ReadbackLocked()
	{
		const bool burst = active && OS::Capture::IsBurst(active->schedule.plan);
		if (burst && ((!active->stopping && (!active->schedule.Finished() || acquiring)) || encodes.size() >= OS::Capture::MaximumQueuedFrames))
			return;
		for (auto it = readbacks.begin(); it != readbacks.end();) {
			bool ready = true;
			bool failed = false;
			winrt::com_ptr<ID3D11DeviceContext> context;
			it->device->GetImmediateContext(context.put());
			if (context.get() != globals::d3d::context)
				throw std::runtime_error("rendering context changed before readback");
			for (uint32_t eye = 0; eye < (it->requiredMask == 3 ? 2u : 1u); ++eye) {
				auto& plane = it->eyes[eye];
				if (!plane.pixels.empty())
					continue;
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const auto result = context->Map(plane.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
					ready = false;
					break;
				}
				if (FAILED(result)) {
					failed = true;
					break;
				}
				struct Unmap
				{
					ID3D11DeviceContext* context;
					ID3D11Texture2D* texture;
					~Unmap() { context->Unmap(texture, 0); }
				} unmap{ context.get(), plane.staging.get() };
				const auto& region = plane.bounds;
				if (!mapped.pData || mapped.RowPitch < region.width * 4) {
					failed = true;
					break;
				}
				const bool bgra = plane.format == DXGI_FORMAT_B8G8R8A8_UNORM || plane.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
				plane.pixels = OS::Capture::CopyPixels(
					{ static_cast<const uint8_t*>(mapped.pData), uint64_t(mapped.RowPitch) * region.height }, mapped.RowPitch, region, bgra);
			}
			if (failed) {
				FinishFrameLocked(*it, "failed", "gpu_readback_failed");
				it = readbacks.erase(it);
			} else if (ready) {
				for (auto& plane : it->eyes)
					plane.staging = nullptr;
				encodes.push_back(std::move(*it));
				it = readbacks.erase(it);
				condition.notify_all();
			} else {
				++it;
			}
			if (burst)
				break;
		}
	}

	void CaptureFlatLocked()
	{
		auto frame = ScheduleLocked(FrameNumber());
		if (!frame)
			return;
		try {
			auto texture = desktopSource ? desktopSource() : nullptr;
			if (!texture) {
				FinishFrameLocked(*frame, "failed", "desktop_source_unavailable");
				return;
			}
			if (!StagePlane(*frame, 0, texture.get(), { 0.f, 0.f, 1.f, 1.f }, vr::ColorSpace_Gamma)) {
				FinishFrameLocked(*frame, "dropped", "readback_memory_limit");
				return;
			}
			frame->session->consecutiveSkips = 0;
			AcquiredLocked(std::move(*frame));
		} catch (const std::exception& error) {
			FinishFrameLocked(*frame, "failed", error.what());
		} catch (...) {
			FinishFrameLocked(*frame, "failed", "unknown_desktop_capture_error");
		}
	}

	void UpdateClockLocked()
	{
		const auto frame = FrameNumber();
		if (active && active->started && frame < active->lastEngineFrame)
			throw std::runtime_error("engine frame counter regressed during capture");
		const bool wasPaused = paused;
		paused = (globals::state && globals::state->isLoadingMenuOpen) || (globals::game::ui && globals::game::ui->GameIsPaused());
		if (active) {
#ifdef DEVBENCH_BRIDGE_ENABLED
			if (paused && active->schedule.plan.burst && active->continuity.acquired && !active->schedule.Finished())
				StopLocked("paused_during_burst");
#endif
			if (active->started) {
				if (paused && !wasPaused)
					active->pausedAtFrame = frame;
				if (!paused && wasPaused)
					active->schedule.nextFrame += frame - active->pausedAtFrame;
			}
			active->lastEngineFrame = frame;
		}
	}

	void Tick(bool enabled)
	{
		renderThread.store(GetCurrentThreadId(), std::memory_order_release);
		if (!capturing.load(std::memory_order_acquire))
			return;
		std::lock_guard lock(mutex);
		if (!enabled)
			StopLocked("feature_disabled");
		UpdateClockLocked();
		ReadbackLocked();
		if (!globals::game::isVR)
			CaptureFlatLocked();
		condition.notify_all();
	}

	json Encode(Frame& frame)
	{
		const auto count = frame.requiredMask == 3 ? 2u : 1u;
		const auto width = frame.eyes[0].bounds.width;
		const auto height = frame.eyes[0].bounds.height;
		std::array<OS::Capture::NativePlane, 2> native;
		json planes = json::array();
		for (uint32_t eye = 0; eye < count; ++eye) {
			native[eye] = { frame.eyes[eye].bounds.width, frame.eyes[eye].bounds.height, frame.eyes[eye].pixels };
			planes.push_back(frame.eyes[eye].metadata);
		}
		const auto image = OS::Capture::CombinePlanes(std::span(native).first(count));
		const auto* pixels = image.GetImage(0, 0, 0);
		const auto& plan = frame.session->schedule.plan;
		auto path = frame.session->directory->Path() / std::format("frame_{:06}.{}", frame.ordinal, plan.png ? "png" : "bmp");
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (!frame.session->referencePath.empty()) {
			path = frame.session->referencePath;
			frame.session->directory->VerifyDestinationChild(path);
		} else
#endif
			frame.session->directory->VerifyDirectChild(path);
		const auto blob = OS::Capture::EncodeImage(*pixels, plan.png);
		const auto artifact = CommittedFile::WriteAtomically(path.wstring() + L".writing", path, blob.GetBufferPointer(), blob.GetBufferSize(), false);
		return { { "path", Util::PathToUtf8(path) }, { "bytes", artifact.bytes }, { "sha256", artifact.sha256 },
			{ "width", width * count }, { "height", height }, { "planes", std::move(planes) } };
	}

	bool ReadyToFinalizeLocked() const
	{
		return active && !active->preparing && !active->finalizing && active->outstanding == 0 && (active->stopping || active->schedule.Finished());
	}

	void WriteManifest(std::shared_ptr<Session> session, json manifest)
	{
		json artifact = nullptr;
		std::string error;
		try {
			const auto path = session->directory->Path() / "manifest.json";
			session->directory->VerifyDirectChild(path);
			const auto content = manifest.dump(2);
			const auto committed = CommittedFile::WriteAtomically(path.wstring() + L".writing", path, content.data(), content.size(), false);
			artifact = { { "path", Util::PathToUtf8(path) }, { "bytes", committed.bytes }, { "sha256", committed.sha256 } };
		} catch (const std::exception& failure) {
			error = failure.what();
		}
		std::unique_lock lock(mutex);
		session->manifest = std::move(artifact);
		if (!error.empty()) {
			session->error = error;
			session->state = "failed";
		} else
			session->state = manifest.at("state").get<std::string>();
		auto directory = std::move(session->directory);
		if (active == session) {
			active.reset();
			capturing.store(false, std::memory_order_release);
		}
		lock.unlock();
		directory.reset();
#ifdef DEVBENCH_BRIDGE_ENABLED
		CompleteReference(session);
#endif
	}

#ifdef DEVBENCH_BRIDGE_ENABLED
	void CompleteReference(const std::shared_ptr<Session>& session) noexcept
	{
		try {
			std::function<void(const json&)> completion;
			json result;
			{
				std::lock_guard lock(mutex);
				completion = std::move(session->referenceCompletion);
				if (!completion)
					return;
				result = OS::Capture::ReferenceCompletion(ReceiptLocked(*session));
			}
			completion(result);
		} catch (...) {
			logger::error("Screenshot reference completion notification failed");
		}
	}
#endif

	void Worker() noexcept
	{
		try {
			WorkerLoop();
		} catch (...) {
			Abort("sequence_worker_failed");
			try {
				std::unique_lock lock(mutex);
				stopRequested = true;
				encodes.clear();
				std::shared_ptr<DirectoryLease> directory;
				if (active) {
					for (auto& frame : active->frames) {
						if (frame["state"] == "acquiring") {
							frame["state"] = "failed";
							frame["reason"] = "sequence_worker_failed";
						}
					}
					active->outstanding = 0;
					active->state = "failed";
					active->error = "sequence_worker_failed";
					directory = std::move(active->directory);
				}
				reservedBytes = 0;
				capturing.store(false, std::memory_order_release);
#ifdef DEVBENCH_BRIDGE_ENABLED
				const auto failed = active;
#endif
				lock.unlock();
				directory.reset();
#ifdef DEVBENCH_BRIDGE_ENABLED
				if (failed)
					CompleteReference(failed);
#endif
			} catch (...) {
				logger::error("Screenshot sequence worker failed without a complete receipt");
			}
		}
	}

	void WorkerLoop()
	{
		const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const SKSE::stl::scope_exit uninitialize([comResult] { if (SUCCEEDED(comResult)) CoUninitialize(); });
		while (true) {
			std::unique_lock lock(mutex);
			condition.wait(lock, [this] { return stopRequested || (active && active->preparing) || !encodes.empty() || ReadyToFinalizeLocked(); });
			if (active && active->preparing) {
				auto session = active;
				if (session->stopping) {
					session->preparing = false;
					session->state = session->error.empty() ? "stopped" : "failed";
					active.reset();
					capturing.store(false, std::memory_order_release);
					lock.unlock();
#ifdef DEVBENCH_BRIDGE_ENABLED
					CompleteReference(session);
#endif
				} else {
					lock.unlock();
					Prepare(session);
				}
				continue;
			}
			if (!encodes.empty()) {
				auto frame = std::move(encodes.front());
				encodes.pop_front();
				lock.unlock();
				json artifact;
				std::string error;
				try {
					CheckResult(comResult);
					artifact = Encode(frame);
				} catch (const std::exception& failure) {
					error = failure.what();
				}
				lock.lock();
				if (error.empty())
					frame.session->frames.at(frame.ordinal - 1)["artifact"] = std::move(artifact);
				FinishFrameLocked(frame, error.empty() ? "written" : "failed", error);
				continue;
			}
			if (ReadyToFinalizeLocked()) {
				auto session = active;
				session->finalizing = true;
				session->state = "finalizing";
				auto manifest = ReceiptLocked(*session);
				manifest["schemaVersion"] = 1;
				manifest["producer"] = Plugin::DISPLAY_NAME;
				manifest["buildDescribe"] = Plugin::BUILD_DESCRIBE;
				manifest["sessionId"] = service.SessionId();
				manifest["capture"] = session->requested;
				manifest["frames"] = session->frames;
				manifest["state"] = !session->error.empty() ? "failed_partial" : session->stoppedByUser                            ? "stopped" :
				                                                             (manifest["failed"] != 0 || manifest["dropped"] != 0) ? "completed_with_warnings" :
				                                                                                                                     "completed";
				lock.unlock();
				WriteManifest(std::move(session), std::move(manifest));
				continue;
			}
			if (stopRequested)
				break;
		}
	}
};

ScreenshotSequenceCapture::ScreenshotSequenceCapture(DesktopSource desktopSource) : impl(std::make_shared<Impl>(std::move(desktopSource))) {}
ScreenshotSequenceCapture::~ScreenshotSequenceCapture() { impl->Shutdown(); }

nlohmann::json ScreenshotSequenceCapture::HandleRequest(const nlohmann::json& request, bool enabled, bool stillCapturePending)
{
	const auto action = request.is_object() ? request.find("action") : request.end();
	const bool readOnly = action != request.end() && (*action == "capabilities" || *action == "status" || *action == "request_get");
	return impl->service.Dispatch(request, [this, enabled, stillCapturePending](const auto& args) { return impl->Dispatch(args, enabled, stillCapturePending); }, [this](std::string_view id) { return impl->Lookup(id); }, !readOnly);
}

#ifdef DEVBENCH_BRIDGE_ENABLED
nlohmann::json ScreenshotSequenceCapture::HandleReferenceRequest(const nlohmann::json& request, bool enabled, bool stillCapturePending,
	std::function<void(const nlohmann::json&)> completion)
{
	if (!completion)
		throw std::invalid_argument("reference capture requires a completion callback");
	const auto command = OS::Capture::ReferenceCommand(request, globals::game::isVR);
	const auto response = impl->service.Dispatch(command, [this, enabled, stillCapturePending, completion](const auto& args) { return impl->Start(args, enabled, stillCapturePending, completion); }, [this](std::string_view id) { return impl->Lookup(id); });
	OS::Capture::ReplayReferenceCompletion(response, completion);
	return response;
}
#endif

void ScreenshotSequenceCapture::Tick(bool enabled)
{
	try {
		impl->Tick(enabled);
	} catch (const std::exception& error) {
		impl->Abort(error.what());
	} catch (...) {
		impl->Abort("unknown_readback_error");
	}
}

void ScreenshotSequenceCapture::Stop()
{
	impl->Abort("feature_disabled");
}

bool ScreenshotSequenceCapture::IsActive() const
{
	return impl->capturing.load(std::memory_order_acquire);
}
