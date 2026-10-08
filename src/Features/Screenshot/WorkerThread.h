#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace OS::Capture
{
	/** Owns one isolated worker; a caller deadline never claims to cancel synchronous filesystem I/O. */
	template <class State>
	class WorkerThread final
	{
	public:
		/** Starts a loop that publishes exited under the shared state mutex before returning. */
		WorkerThread(std::shared_ptr<State> a_state, void (*a_loop)(std::shared_ptr<State>)) :
			state(std::move(a_state)), worker(a_loop, state)
		{}
		WorkerThread(const WorkerThread&) = delete;
		WorkerThread& operator=(const WorkerThread&) = delete;
		~WorkerThread() { ShutdownUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2)); }

		/** Close admission; the worker retains its state until queued work and any active call finish. */
		void RequestStop()
		{
			{
				std::lock_guard lock(state->mutex);
				state->stopRequested = true;
			}
			state->condition.notify_all();
		}

		/** Join a completed worker or detach its shared state at the common caller deadline. */
		bool ShutdownUntil(std::chrono::steady_clock::time_point a_deadline)
		{
			if (!worker.joinable())
				return joined;
			RequestStop();
			bool exited;
			{
				std::unique_lock lock(state->mutex);
				exited = state->condition.wait_until(lock, a_deadline, [&] { return state->exited; });
			}
			if (exited)
				worker.join();
			else
				worker.detach();
			joined = exited;
			return exited;
		}

	private:
		std::shared_ptr<State> state;
		std::thread worker;
		bool joined = false;
	};
}
