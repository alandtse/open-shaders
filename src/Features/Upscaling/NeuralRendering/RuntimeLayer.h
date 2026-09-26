#pragma once

#include "D3D12Interop.h"
#include "Lifecycle.h"
#include "PendingRequest.h"
#include "Runtime.h"

#include <filesystem>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace NR
{
	/** @brief Last result of looking for the runtime DLL; refreshed on every lifecycle request. */
	struct DllProbe
	{
		bool found = false;
		std::string version;
		bool versionAccepted = false;
	};

	/**
	 * @brief Owns the NR runtime's process-wide state and its request-driven lifecycle.
	 *        Nothing starts on its own: while no caller requests an attempt the layer does no
	 *        D3D or NGX work at all, and a failure latches instead of retrying.
	 */
	class RuntimeLayer
	{
	public:
		RuntimeLayer() = default;
		~RuntimeLayer() = default;
		RuntimeLayer(const RuntimeLayer&) = delete;
		RuntimeLayer& operator=(const RuntimeLayer&) = delete;

		/** @brief Queues a request for the render thread; replaces one that has not been taken. */
		void Post(RequestKind a_kind) { pending.Post(a_kind); }

		/**
		 * @brief Performs the queued request, if any. Call once per frame from the render thread.
		 *        With nothing queued this is a single relaxed atomic load, so the per-frame cost
		 *        is nil on the frames between requests.
		 */
		void ProcessPendingRequest();

		/** @brief NR status for devbench: DLL presence, version gate, state, reason, retry count, pending request. */
		[[nodiscard]] nlohmann::json Status();

	private:
		/**
		 * @brief Makes one attempt: probe, version gate, interop device, then NGX.
		 * @return false when the attempt was not permitted (the layer is not idle, or its retry
		 *         budget is spent) or it failed and latched (one log line).
		 */
		bool RequestInitialize();
		/**
		 * @brief Re-arms after a recoverable failure and attempts again.
		 *        A failure retained by a failed drain is drained first instead of being re-reported.
		 * @return false when no attempt may start: the layer is live, the failure is permanent,
		 *         or the retry budget is spent; the latch and its reason are then left as they are.
		 */
		bool RequestRetry();
		/**
		 * @brief Drains, then releases the runtime and the interop device.
		 * @return false when the drain failed; the runtime and its resources are then kept alive.
		 */
		bool RequestShutdown();

		/** @brief Streamline plugin directory holding the runtime DLL, from the game's Data path. */
		const std::filesystem::path& Directory();
		/** @brief Re-reads the runtime DLL's presence and version; never per frame. */
		void RefreshProbe();
		/** @brief Records a latched failure and emits its single log line at the matching severity. */
		void Latch(FailureKind a_kind, std::string a_reason);

		D3D12Interop interop;
		Runtime runtime;
		Lifecycle lifecycle;
		DllProbe probe;
		bool probed = false;
		std::string lastReason;
		std::filesystem::path directory;
		PendingRequest pending;
		/** Serializes a request the render thread is performing against Status() on a caller thread. */
		std::mutex runtimeMutex;
		TerminationSentinel terminationSentinel;  ///< last, so it is destroyed before the NGX teardown above
	};
}
