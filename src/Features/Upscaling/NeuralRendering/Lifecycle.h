#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace NR
{
	/** @brief DLL major version the Feature 18 parameter ABI was observed against. */
	inline constexpr uint16_t kRuntimeVersionMajor = 310;
	/** @brief DLL minor version the Feature 18 parameter ABI was observed against. */
	inline constexpr uint16_t kRuntimeVersionMinor = 8;
	/** @brief NGX Feature 18 snippet, expected in the Streamline plugin directory. */
	inline constexpr const wchar_t* kRuntimeDllName = L"nvngx_dlssnr.dll";

	/** @brief Lifecycle state of the NR runtime layer. */
	enum class RuntimeState : uint8_t
	{
		kNotLoaded,    ///< Idle: no attempt has run; an attempt starts here, so the states before kInitialized are unobservable.
		kInitialized,  ///< NGX accepted the device and both parameter blocks exist.
		kFailed        ///< Latched off; only an explicit retry clears it.
	};

	/** @brief Why an attempt stopped; selects the log severity and the diagnostics reason. */
	enum class FailureKind : uint8_t
	{
		kNone,
		kDllMissing,
		kVersionRejected,
		kLoadFailed,
		kExportMissing,
		kUnsupportedAdapter,
		kInitFailed,
		kParameterApiUnavailable,
		kSehFault,
		kDeviceRemoved,
		kDrainTimeout,
		kWaitFailed
	};

	/** @brief What an attempt does about the runtime DLL probe, before any D3D12 or NGX work. */
	enum class ProbeDecision : uint8_t
	{
		kDllMissing,       ///< Nothing to load: the runtime DLL is not installed.
		kVersionRejected,  ///< Present but not a supported line, so no D3D12 device may be created for it.
		kProceed           ///< Present and supported: device and NGX work may start.
	};

	/** @brief What a retry request may do, from the current latch and the retry budget. */
	enum class RetryDecision : uint8_t
	{
		kAttempt,     ///< Nothing is latched: the request is a plain attempt.
		kRearm,       ///< A recoverable latch with budget left: re-arm for one more attempt.
		kDrainFirst,  ///< A failed shutdown kept the runtime alive: the drain must retire first.
		kRefused      ///< Not idle, poisoned, or out of retries: no attempt may start.
	};

	/** @brief Severity of the single log line a latched failure emits. */
	enum class FailureSeverity : uint8_t
	{
		kNone,
		kWarn,
		kError
	};

	/** @brief A failure tagged with the lifecycle category the caller must latch. */
	struct RuntimeError : std::runtime_error
	{
		RuntimeError(FailureKind a_kind, std::string a_message) :
			std::runtime_error(std::move(a_message)), kind(a_kind) {}

		FailureKind kind;
	};

	/**
	 * @brief True when a readable DLL version is a supported 310.8.x build.
	 *        Patch and build are ignored, so any 310.8.x passes.
	 * @tparam Version Any type exposing major()/minor() (e.g. REL::Version).
	 */
	template <class Version>
	[[nodiscard]] constexpr bool IsSupportedRuntimeVersion(const std::optional<Version>& a_found)
	{
		return a_found && a_found->major() == kRuntimeVersionMajor && a_found->minor() == kRuntimeVersionMinor;
	}

	/**
	 * @brief What an attempt does about the DLL probe, decided before any D3D12 or NGX work.
	 *        A present but unsupported DLL is refused here, so no device is ever created for it.
	 */
	[[nodiscard]] constexpr ProbeDecision DecideProbe(bool a_dllFound, bool a_versionAccepted)
	{
		if (!a_dllFound)
			return ProbeDecision::kDllMissing;
		if (!a_versionAccepted)
			return ProbeDecision::kVersionRejected;
		return ProbeDecision::kProceed;
	}

	/** @brief An absent DLL is an expected configuration (warn); every other failure is an error. */
	[[nodiscard]] constexpr FailureSeverity FailureSeverityOf(FailureKind a_kind)
	{
		if (a_kind == FailureKind::kNone)
			return FailureSeverity::kNone;
		return a_kind == FailureKind::kDllMissing ? FailureSeverity::kWarn : FailureSeverity::kError;
	}

	/** @brief True when a failure poisoned the process: NGX is never re-entered, only a restart clears it. */
	[[nodiscard]] constexpr bool IsTerminalFailure(FailureKind a_kind)
	{
		return a_kind == FailureKind::kSehFault;
	}

	/**
	 * @brief True when a failure left the runtime and its resources alive, so only a later
	 *        successful drain of the same layer may release them.
	 */
	[[nodiscard]] constexpr bool IsRetainedFailure(FailureKind a_kind)
	{
		return a_kind == FailureKind::kDrainTimeout || a_kind == FailureKind::kWaitFailed || a_kind == FailureKind::kDeviceRemoved;
	}

	/**
	 * @brief The reason a DLL version was refused; the pre-device gate and the runtime's own
	 *        backstop both report this text, so one rejected DLL reads the same either way.
	 */
	[[nodiscard]] inline std::string VersionRejectedReason(std::string_view a_fileName, std::string_view a_directory, std::string_view a_version)
	{
		return std::format("{} in {} is {}, need {}.{}.x", a_fileName, a_directory,
			a_version.empty() ? "missing version info" : a_version, kRuntimeVersionMajor, kRuntimeVersionMinor);
	}

	/** @brief Diagnostics label for a lifecycle state. */
	[[nodiscard]] constexpr std::string_view StateName(RuntimeState a_state)
	{
		switch (a_state) {
		case RuntimeState::kNotLoaded:
			return "NotLoaded";
		case RuntimeState::kInitialized:
			return "Initialized";
		case RuntimeState::kFailed:
			return "Failed";
		}
		return "Unknown";
	}

	/** @brief Diagnostics label for a failure category. */
	[[nodiscard]] constexpr std::string_view FailureKindName(FailureKind a_kind)
	{
		switch (a_kind) {
		case FailureKind::kNone:
			return "None";
		case FailureKind::kDllMissing:
			return "MissingDll";
		case FailureKind::kVersionRejected:
			return "VersionRejected";
		case FailureKind::kLoadFailed:
			return "LoadFailed";
		case FailureKind::kExportMissing:
			return "ExportMissing";
		case FailureKind::kUnsupportedAdapter:
			return "UnsupportedAdapter";
		case FailureKind::kInitFailed:
			return "InitFailed";
		case FailureKind::kParameterApiUnavailable:
			return "ParameterApiUnavailable";
		case FailureKind::kSehFault:
			return "SehFault";
		case FailureKind::kDeviceRemoved:
			return "DeviceRemoved";
		case FailureKind::kDrainTimeout:
			return "DrainTimeout";
		case FailureKind::kWaitFailed:
			return "WaitFailed";
		}
		return "Unknown";
	}

	/**
	 * @brief Attempt and latch state for the runtime layer: one attempt per explicit request.
	 *        Nothing latches back to an attemptable state on its own, so no attempt can run per frame.
	 */
	class Lifecycle
	{
	public:
		/** @brief Retries allowed per process before the latch is permanent. */
		static constexpr uint32_t kMaxRetries = 3;

		[[nodiscard]] RuntimeState State() const { return state; }
		[[nodiscard]] FailureKind Failure() const { return failure; }
		[[nodiscard]] uint32_t RetryCount() const { return retryCount; }

		/**
		 * @brief True when an attempt may start: idle, with either budget left or the arm of a retry
		 *        that spent the last of it. Only the attempt that arm was granted for consumes it.
		 */
		[[nodiscard]] constexpr bool CanAttempt() const { return state == RuntimeState::kNotLoaded && (rearmed || retryCount < kMaxRetries); }

		/** @brief Records a live NGX runtime with both parameter blocks allocated. */
		void MarkInitialized()
		{
			state = RuntimeState::kInitialized;
			failure = FailureKind::kNone;
			rearmed = false;
		}

		/** @brief Latches the layer off; no further attempt runs until RequestRetry permits one. */
		void MarkFailed(FailureKind a_kind)
		{
			state = RuntimeState::kFailed;
			failure = a_kind;
			rearmed = false;
		}

		/**
		 * @brief What a retry request may do, from the current latch and the retry budget.
		 *        The budget refuses only here, so the attempt a retry has paid for still runs.
		 */
		[[nodiscard]] constexpr RetryDecision DecideRetry() const
		{
			if (state != RuntimeState::kFailed)
				return CanAttempt() ? RetryDecision::kAttempt : RetryDecision::kRefused;
			if (IsTerminalFailure(failure) || retryCount >= kMaxRetries)
				return RetryDecision::kRefused;
			return IsRetainedFailure(failure) ? RetryDecision::kDrainFirst : RetryDecision::kRearm;
		}

		/** @brief Re-arms a latched layer for one more attempt; false when unrecoverable or out of retries. */
		[[nodiscard]] bool RequestRetry()
		{
			const auto decision = DecideRetry();
			if (decision != RetryDecision::kRearm && decision != RetryDecision::kDrainFirst)
				return false;
			++retryCount;
			rearmed = true;
			failure = FailureKind::kNone;
			state = RuntimeState::kNotLoaded;
			return true;
		}

		/**
		 * @brief Drops back to idle after a completed shutdown, so a later request can attempt again.
		 *        The retry budget survives: a completed shutdown releases resources, it does not
		 *        refund the retries a failed layer already spent. An arm survives too, because the
		 *        drain-first retry that set it has not run its attempt yet.
		 */
		void Reset()
		{
			state = RuntimeState::kNotLoaded;
			failure = FailureKind::kNone;
		}

	private:
		RuntimeState state = RuntimeState::kNotLoaded;
		FailureKind failure = FailureKind::kNone;
		uint32_t retryCount = 0;
		/** Set by the retry that spends the last of the budget; the attempt it was granted clears it. */
		bool rearmed = false;
	};
}
