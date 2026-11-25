#include "Features/DX12/DX12FenceRetirementPolicy.h"
#include "Utils/RuntimeTaskGate.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

TEST_CASE("Runtime task admission shares the shader concurrency budget", "[dx12][runtime-task]")
{
	using Util::CanAdmitRuntimeTask;

	CHECK(CanAdmitRuntimeTask(false, false, false, 0, 1));
	CHECK(CanAdmitRuntimeTask(false, false, false, 3, 4));
	CHECK_FALSE(CanAdmitRuntimeTask(false, false, false, 4, 4));
	CHECK_FALSE(CanAdmitRuntimeTask(false, false, false, 0, 0));
	CHECK_FALSE(CanAdmitRuntimeTask(false, false, false, 0, -1));
	CHECK_FALSE(CanAdmitRuntimeTask(false, false, true, 0, 4));
	CHECK_FALSE(CanAdmitRuntimeTask(false, true, false, 0, 4));
	CHECK_FALSE(CanAdmitRuntimeTask(true, false, false, 0, 4));
}

TEST_CASE("A closed runtime gate rejects late fence continuations", "[dx12][runtime-task]")
{
	int admitted = 0;
	Util::RuntimeTaskGate gate([&](Util::RuntimeTaskGate::Task) {
		++admitted;
		return true;
	});
	CHECK_FALSE(gate.TrySubmit({}));
	CHECK(gate.TrySubmit([] {}));
	gate.Close();
	CHECK_FALSE(gate.TrySubmit([] {}));
	gate.Close();
	CHECK_FALSE(gate.TrySubmit([] {}));
	CHECK(admitted == 1);
}

TEST_CASE("Closing a runtime gate serializes against concurrent submitters", "[dx12][runtime-task]")
{
	std::atomic<bool> closed = false;
	std::atomic<bool> submittedAfterClose = false;
	Util::RuntimeTaskGate gate([&](Util::RuntimeTaskGate::Task) {
		if (closed.load(std::memory_order_acquire))
			submittedAfterClose.store(true, std::memory_order_release);
		return true;
	});
	std::latch ready(5);
	std::vector<std::jthread> submitters;
	for (int i = 0; i < 4; ++i) {
		submitters.emplace_back([&] {
			ready.arrive_and_wait();
			for (int attempt = 0; attempt < 256; ++attempt)
				(void)gate.TrySubmit([] {});
		});
	}
	ready.arrive_and_wait();
	gate.Close();
	closed.store(true, std::memory_order_release);
	for (auto& thread : submitters)
		thread.join();
	CHECK_FALSE(submittedAfterClose.load(std::memory_order_acquire));
	CHECK_FALSE(gate.TrySubmit([] {}));
}

TEST_CASE("GPU retirement requires a real completion or device removal", "[dx12][fence]")
{
	using DX12Native::FenceValueRetired;
	using DX12Native::kUntrackedFenceValue;

	CHECK(FenceValueRetired(false, 0, 0));
	CHECK_FALSE(FenceValueRetired(false, kUntrackedFenceValue, 1));
	CHECK_FALSE(FenceValueRetired(true, 4, 5));
	CHECK(FenceValueRetired(true, 5, 5));
	CHECK(FenceValueRetired(true, 6, 5));
	CHECK_FALSE(FenceValueRetired(true, kUntrackedFenceValue - 1, kUntrackedFenceValue));
	CHECK(FenceValueRetired(true, kUntrackedFenceValue, 5));
	CHECK(FenceValueRetired(true, kUntrackedFenceValue, kUntrackedFenceValue));
}
