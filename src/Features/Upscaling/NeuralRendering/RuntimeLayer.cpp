#include "RuntimeLayer.h"

#include "Utils/FileSystem.h"
#include "Utils/WinApi.h"

#include <format>

namespace NR
{
	const std::filesystem::path& RuntimeLayer::Directory()
	{
		if (directory.empty())
			directory = (Util::PathHelpers::GetDataPath() / "Shaders" / "Upscaling" / "Streamline").lexically_normal();
		return directory;
	}

	void RuntimeLayer::RefreshProbe()
	{
		const auto path = Directory() / kRuntimeDllName;
		std::error_code error;
		probe = {};
		probe.found = std::filesystem::is_regular_file(path, error);
		probed = true;
		if (!probe.found)
			return;
		const auto version = Util::GetDllVersion(path.wstring());
		if (version)
			probe.version = version->string();
		probe.versionAccepted = IsSupportedRuntimeVersion(version);
	}

	void RuntimeLayer::Latch(FailureKind a_kind, std::string a_reason)
	{
		lifecycle.MarkFailed(a_kind);
		lastReason = std::move(a_reason);
		if (FailureSeverityOf(a_kind) == FailureSeverity::kWarn)
			logger::warn("[NeuralRendering] {}; neural rendering is off", lastReason);
		else
			logger::error("[NeuralRendering] {}; neural rendering is off", lastReason);
	}

	bool RuntimeLayer::RequestInitialize()
	{
		if (!lifecycle.CanAttempt())
			return false;
		RefreshProbe();
		const auto path = Directory() / kRuntimeDllName;
		// Converted here because path::string() throws for a name outside the ANSI code page, and
		// this runs on the render thread where a throw would unwind the frame's image-space pass.
		const auto fileName = stl::utf16_to_utf8(path.filename().wstring()).value_or("<unprintable path>");
		const auto directoryText = stl::utf16_to_utf8(Directory().wstring()).value_or("<unprintable path>");
		switch (DecideProbe(probe.found, probe.versionAccepted)) {
		case ProbeDecision::kDllMissing:
			Latch(FailureKind::kDllMissing, std::format("{} is not installed in {}", fileName, directoryText));
			return false;
		case ProbeDecision::kVersionRejected:
			// Refused before D3D12Interop::Initialize, so a wrong-version DLL never gets a device.
			Latch(FailureKind::kVersionRejected, VersionRejectedReason(fileName, directoryText, probe.version));
			return false;
		case ProbeDecision::kProceed:
			break;
		}

		AttemptResult result;
		try {
			interop.Initialize();
			result = runtime.Initialize(interop, Directory());
		} catch (const RuntimeError& e) {
			result.kind = e.kind;
			result.reason = e.what();
		} catch (const std::exception& e) {
			result.kind = FailureKind::kInitFailed;
			result.reason = e.what();
		} catch (const winrt::hresult_error& e) {
			// winrt::hresult_error is not a std::exception, so it needs its own latch.
			result.kind = FailureKind::kInitFailed;
			result.reason = winrt::to_string(e.message());
		}

		if (!result.ok) {
			if (!IsTerminalFailure(result.kind))
				interop.Reset();  // no NGX runtime is using the device, so do not hold it
			Latch(result.kind, std::move(result.reason));
			return false;
		}
		lastReason.clear();
		lifecycle.MarkInitialized();
		return true;
	}

	bool RuntimeLayer::RequestRetry()
	{
		switch (lifecycle.DecideRetry()) {
		case RetryDecision::kRefused:
			return false;  // refused without touching the latch, so its failure and reason stay readable
		case RetryDecision::kDrainFirst:
			// The failed drain kept the runtime and its resources alive, so this retry drains again
			// before it may attempt; it spends budget like any other.
			return lifecycle.RequestRetry() && RequestShutdown() && RequestInitialize();
		case RetryDecision::kRearm:
			return lifecycle.RequestRetry() && RequestInitialize();
		case RetryDecision::kAttempt:
			break;
		}
		return RequestInitialize();
	}

	bool RuntimeLayer::RequestShutdown()
	{
		if (processTerminating.load(std::memory_order_relaxed))
			return true;
		// The poison must be read before Shutdown drops the runtime, and a fault that never
		// committed an Impl survives only in the lifecycle, so check both.
		const bool poisoned = runtime.Poisoned() || IsTerminalFailure(lifecycle.Failure());
		auto result = runtime.Shutdown();
		if (!result.ok) {
			Latch(result.kind, std::move(result.reason));
			return false;
		}
		interop.Reset();
		if (poisoned) {
			lifecycle.MarkFailed(FailureKind::kSehFault);
			return true;
		}
		if (result.kind != FailureKind::kNone) {
			// The resources are released, but a clean idle layer would hide that the GPU was gone:
			// latch the removal so status reports it, and a retry re-arms from there.
			Latch(result.kind, std::move(result.reason));
			return true;
		}
		lastReason.clear();
		lifecycle.Reset();
		return true;
	}

	void RuntimeLayer::ProcessPendingRequest()
	{
		if (!pending.Any())
			return;
		std::scoped_lock lock(runtimeMutex);
		// Nothing may escape into the engine's render thunk, so a fault from any request latches
		// the layer here instead of unwinding the frame that called it.
		try {
			switch (pending.Take()) {
			case RequestKind::kInitialize:
				RequestInitialize();
				break;
			case RequestKind::kRetry:
				RequestRetry();
				break;
			case RequestKind::kShutdown:
				RequestShutdown();
				break;
			case RequestKind::kNone:
				break;
			}
		} catch (const winrt::hresult_error& e) {
			Latch(FailureKind::kInitFailed, winrt::to_string(e.message()));
		} catch (const std::exception& e) {
			Latch(FailureKind::kInitFailed, e.what());
		} catch (...) {
			Latch(FailureKind::kInitFailed, "an unhandled fault escaped the runtime request");
		}
	}

	nlohmann::json RuntimeLayer::Status()
	{
		std::scoped_lock lock(runtimeMutex);
		if (!probed)
			RefreshProbe();
		const auto path = Directory() / kRuntimeDllName;
		return nlohmann::json{
			{ "dllPath", stl::utf16_to_utf8(path.wstring()).value_or("<unprintable path>") },
			{ "dllDirectory", stl::utf16_to_utf8(Directory().wstring()).value_or("<unprintable path>") },
			{ "dllFound", probe.found },
			{ "version", probe.version },
			{ "versionAccepted", probe.versionAccepted },
			{ "requiredVersion", std::format("{}.{}.x", kRuntimeVersionMajor, kRuntimeVersionMinor) },
			{ "state", std::string(StateName(lifecycle.State())) },
			{ "failure", std::string(FailureKindName(lifecycle.Failure())) },
			{ "lastError", lastReason },
			{ "retryCount", lifecycle.RetryCount() },
			{ "maxRetries", Lifecycle::kMaxRetries },
			{ "pendingRequest", std::string(RequestKindName(pending.Peek())) },
			{ "runtimeInitialized", runtime.Initialized() },
			{ "runtimePoisoned", runtime.Poisoned() },
			{ "interopInitialized", interop.Initialized() },
			{ "submittedFence", interop.Submitted() },
			{ "completedFence", interop.Completed() },
		};
	}
}
