#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace Util
{
	/** @brief Hands off callbacks from any producer to the render thread in detached batches. */
	class RenderTaskQueue
	{
	public:
		/** @brief Queues a callback without executing it on the calling thread. */
		void Enqueue(std::function<void()> task)
		{
			std::lock_guard lock(mutex);
			tasks.push_back(std::move(task));
			pending.store(true, std::memory_order_release);
		}

		/** @brief Takes the current batch so callbacks can execute without holding the queue lock. */
		std::vector<std::function<void()>> TakePending()
		{
			if (!pending.load(std::memory_order_acquire))
				return {};
			std::vector<std::function<void()>> batch;
			std::lock_guard lock(mutex);
			batch.swap(tasks);
			pending.store(false, std::memory_order_release);
			return batch;
		}

	private:
		std::mutex mutex;
		std::vector<std::function<void()>> tasks;
		std::atomic<bool> pending{ false };
	};
}
