#pragma once
#include <Windows.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <wrl/wrappers/corewrappers.h>

namespace CS::Redstone
{
	class InputPacing
	{
		using Clock = std::chrono::steady_clock;
		Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> timer_;
		std::mutex mutex_;
		Clock::time_point nextInput_{};
		unsigned pacedFPS_ = 0;
		bool timerAttempted_ = false;

	public:
		/** @brief Wait only for the frame budget left after Anti-Lag, before engine input polling. */
		[[nodiscard]] std::chrono::microseconds Apply(bool sceneBackstop, unsigned realFPS)
		{
			std::lock_guard lock(mutex_);
			const auto start = Clock::now();
			std::chrono::microseconds waited{};
			if (!sceneBackstop || !realFPS) {
				nextInput_ = {};
				pacedFPS_ = 0;
				return waited;
			}
			constexpr auto kNanosecondsPerSecond = 1000000000LL;
			const auto period = std::chrono::nanoseconds((kNanosecondsPerSecond + realFPS - 1) / realFPS);
			if (nextInput_ == Clock::time_point{} || pacedFPS_ != realFPS) {
				nextInput_ = start + period;
				pacedFPS_ = realFPS;
				return waited;
			}
			const auto deadline = nextInput_;
			if (start < deadline) {
				if (!timerAttempted_) {
					timerAttempted_ = true;
					timer_.Attach(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
					if (!timer_.IsValid())
						timer_.Attach(CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
				}
				constexpr auto kNanosecondsPerTimerTick = 100LL;
				constexpr DWORD kMaximumTimerWaitMs = 20;
				const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
				if (timer_.IsValid() && remaining > 0) {
					LARGE_INTEGER due{};
					due.QuadPart = -(remaining + kNanosecondsPerTimerTick - 1) / kNanosecondsPerTimerTick;
					if (SetWaitableTimerEx(timer_.Get(), &due, 0, nullptr, nullptr, nullptr, 0))
						WaitForSingleObject(timer_.Get(), kMaximumTimerWaitMs);
				}
				if (Clock::now() < deadline)
					std::this_thread::sleep_until(deadline);
				waited = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
			}
			const auto now = Clock::now();
			nextInput_ = now >= deadline + period ? now + period : deadline + period;
			return waited;
		}
	};
}
