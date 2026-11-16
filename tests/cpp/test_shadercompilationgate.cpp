#include "Utils/ShaderCompilationGate.h"

#include <atomic>
#include <thread>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Disk cache permits source compilation only during startup", "[shadercompilationgate]")
{
	Util::ShaderCompilationGate gate;
	CHECK(gate.IsDiskCache());
	CHECK(gate.Acquire().owns_lock());

	gate.FinishStartup();
	CHECK(gate.IsReadOnly());
	CHECK_FALSE(gate.Acquire().owns_lock());

	gate.SetDiskCache(false);
	CHECK(gate.Acquire().owns_lock());
	gate.SetDiskCache(true);
	CHECK_FALSE(gate.Acquire().owns_lock());
}

TEST_CASE("The startup boundary drains admitted writes and rejects new work", "[shadercompilationgate]")
{
	Util::ShaderCompilationGate gate;
	auto admitted = gate.Acquire();
	REQUIRE(admitted.owns_lock());

	std::atomic_bool startupReturned{ false };
	std::jthread finish([&] {
		gate.FinishStartup();
		startupReturned = true;
	});
	while (!gate.IsReadOnly())
		std::this_thread::yield();

	CHECK_FALSE(gate.Acquire().owns_lock());
	CHECK_FALSE(startupReturned.load());
	admitted.unlock();
	finish.join();
	CHECK(startupReturned.load());
}

TEST_CASE("Finishing startup leaves diskless background compilation running", "[shadercompilationgate]")
{
	Util::ShaderCompilationGate gate;
	gate.SetDiskCache(false);
	auto admitted = gate.Acquire();
	REQUIRE(admitted.owns_lock());
	gate.FinishStartup();
	CHECK_FALSE(gate.IsReadOnly());
}
