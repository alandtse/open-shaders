#pragma once

#include <cstdint>

namespace SplitLatchBackoff
{
	inline constexpr uint32_t kInitialRetryFrames = 600;
	inline constexpr uint32_t kMaxRetryFrames = 9600;

	/** @brief True when a latched split cache has waited its retry backoff, or when the frame counter went backwards. */
	[[nodiscard]] constexpr bool RetryDue(uint32_t a_now, uint32_t a_latchedFrame, uint32_t a_retryFrames) noexcept
	{
		if (a_now < a_latchedFrame)
			return true;
		return a_now - a_latchedFrame >= a_retryFrames;
	}

	/** @brief Doubles the retry backoff, clamped to kMaxRetryFrames before the multiply so it cannot overflow. */
	[[nodiscard]] constexpr uint32_t NextRetryFrames(uint32_t a_retryFrames) noexcept
	{
		return a_retryFrames >= kMaxRetryFrames / 2 ? kMaxRetryFrames : a_retryFrames * 2;
	}
}
