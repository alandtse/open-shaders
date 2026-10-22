#include "Features/Screenshot/WorkerThread.h"
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <future>

namespace
{
	struct WorkerState
	{
		std::mutex mutex;
		std::condition_variable condition;
		bool stopRequested = false;
		bool exited = false;
		std::shared_future<void> release;
		std::promise<void> finished;
	};

	void Drain(std::shared_ptr<WorkerState> state)
	{
		if (state->release.valid())
			state->release.wait();
		{
			std::unique_lock lock(state->mutex);
			state->condition.wait(lock, [&] { return state->stopRequested; });
			state->exited = true;
		}
		state->condition.notify_all();
		state->finished.set_value();
	}
}

TEST_CASE("Capture shutdown joins an idle worker", "[screenshot-worker]")
{
	auto state = std::make_shared<WorkerState>();
	OS::Capture::WorkerThread<WorkerState> worker(state, &Drain);
	CHECK(worker.ShutdownUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
	CHECK(state->stopRequested);
	CHECK(state->exited);
	CHECK(worker.ShutdownUntil(std::chrono::steady_clock::now()));
}

TEST_CASE("Capture shutdown detaches blocked I/O while retaining its state", "[screenshot-worker]")
{
	std::promise<void> release;
	auto state = std::make_shared<WorkerState>();
	state->release = release.get_future().share();
	auto finished = state->finished.get_future();
	std::weak_ptr<WorkerState> retained = state;
	{
		OS::Capture::WorkerThread<WorkerState> worker(state, &Drain);
		CHECK_FALSE(worker.ShutdownUntil(std::chrono::steady_clock::now()));
		CHECK(state->stopRequested);
		CHECK_FALSE(state->exited);
		state.reset();
	}
	CHECK_FALSE(retained.expired());
	release.set_value();
	REQUIRE(finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}
