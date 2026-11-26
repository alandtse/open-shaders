#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace Util
{
	/** @brief Lifetime-safe access to the existing runtime task dispatcher.
	 * Fence callbacks may outlive the feature, but cannot call a closed dispatcher. */
	class RuntimeTaskGate
	{
	public:
		using Task = std::function<void()>;
		using Submitter = std::function<bool(Task)>;

		explicit RuntimeTaskGate(Submitter submit) : submitter(std::move(submit)) {}

		bool TrySubmit(Task task)
		{
			std::scoped_lock lock(mutex);
			return submitter && task && submitter(std::move(task));
		}

		void Close()
		{
			std::scoped_lock lock(mutex);
			submitter = {};
		}

	private:
		std::mutex mutex;
		Submitter submitter;
	};

	// Render-demanded shaders always win admission. One runtime task may occupy
	// the shared budget, including across a scene reset or feature re-enable.
	constexpr bool CanAdmitRuntimeTask(bool closed, bool runtimeInFlight, bool demandedShaders,
		uint32_t inFlight, int32_t limit)
	{
		return !closed && !runtimeInFlight && !demandedShaders && limit > 0 &&
		       inFlight < static_cast<uint32_t>(limit);
	}
}
