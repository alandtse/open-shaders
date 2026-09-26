#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace NR
{
	/** @brief A lifecycle action a caller queues for the render thread to perform. */
	enum class RequestKind : uint8_t
	{
		kNone,        ///< Nothing queued.
		kInitialize,  ///< One initialization attempt.
		kRetry,       ///< Re-arm after a failure and attempt again.
		kShutdown     ///< Drain, then release the runtime and the interop device.
	};

	/** @brief Diagnostics label for a queued request. */
	[[nodiscard]] constexpr std::string_view RequestKindName(RequestKind a_kind)
	{
		switch (a_kind) {
		case RequestKind::kNone:
			return "None";
		case RequestKind::kInitialize:
			return "Initialize";
		case RequestKind::kRetry:
			return "Retry";
		case RequestKind::kShutdown:
			return "Shutdown";
		}
		return "Unknown";
	}

	/**
	 * @brief One-slot mailbox from a request thread to the render thread.
	 *        Posting replaces a request that has not been taken yet, so a burst collapses to its
	 *        newest request and a queued init never runs once a shutdown has been posted after it.
	 */
	class PendingRequest
	{
	public:
		/** @brief Queues a request, replacing one that has not been taken. */
		void Post(RequestKind a_kind) { kind.store(a_kind, std::memory_order_release); }

		/** @brief Takes the queued request, leaving none; the render thread owns this call. */
		[[nodiscard]] RequestKind Take() { return kind.exchange(RequestKind::kNone, std::memory_order_acq_rel); }

		/** @brief The queued request without taking it, so a status read reports what is still waiting. */
		[[nodiscard]] RequestKind Peek() const { return kind.load(std::memory_order_relaxed); }

		/** @brief Whether anything is queued; the per-frame fast path, before any lock is taken. */
		[[nodiscard]] bool Any() const { return Peek() != RequestKind::kNone; }

	private:
		std::atomic<RequestKind> kind{ RequestKind::kNone };
	};
}
