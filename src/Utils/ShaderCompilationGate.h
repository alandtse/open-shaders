#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace Util
{
	/** Allows source compilation and bytecode writes only during startup when disk caching is enabled. */
	class ShaderCompilationGate
	{
	public:
		/** Keeps the current compile window open for the lifetime of an admitted operation. */
		[[nodiscard]] std::shared_lock<std::shared_mutex> Acquire() const
		{
			if (IsReadOnly())
				return {};
			std::shared_lock lock(mutex);
			if (IsReadOnly())
				return {};
			return lock;
		}

		/** Latches the end of startup and drains any admitted work before returning. */
		void FinishStartup()
		{
			startupFinished.store(true, std::memory_order_release);
			if (IsDiskCache()) {
				std::unique_lock lock(mutex);
			}
		}

		/** Changes the policy after draining admitted work. */
		void SetDiskCache(bool a_enabled)
		{
			if (a_enabled == IsDiskCache())
				return;
			std::unique_lock lock(mutex);
			diskCache.store(a_enabled, std::memory_order_relaxed);
		}

		/** Gets the disk-cache setting independently of the current compilation window. */
		bool IsDiskCache() const { return diskCache.load(std::memory_order_relaxed); }

		/** Whether cache hits are allowed but misses cannot compile. */
		bool IsReadOnly() const { return startupFinished.load(std::memory_order_acquire) && IsDiskCache(); }

	private:
		mutable std::shared_mutex mutex;
		std::atomic_bool diskCache{ true };
		std::atomic_bool startupFinished{ false };
	};
}
