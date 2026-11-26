#include "Features/DX12/DX12FenceRetirementPolicy.h"
#include "Features/DX12/DX12CasterCoverage.h"
#include "Features/DX12/SpatialGrid.h"
#include "Utils/RuntimeTaskGate.h"
#include "../../features/DX12 Native Scene/Shaders/DX12NativeScene/NativeShadowSampling.hlsli"

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

TEST_CASE("DX12 grid sizes and predictive ordering retain their existing contract", "[dx12][grid]")
{
	constexpr std::array<uint32_t, 7> expected{ 4096, 6144, 8192, 10240, 12288, 14336, 16384 };
	CHECK(DX12::kSubcellSizes == expected);
	CHECK(DX12::kGridCapacity == 9);
	CHECK(DX12::kPredictiveCapacity == 16);
	CHECK(DX12::kPredictiveSideCount == 12);

	const auto active = DX12::Neighborhood(1, DX12::kMinSubcellSize, -1.0, 0.0);
	const auto predictive = DX12::PredictiveNeighborhood(1, DX12::kMinSubcellSize, -1.0, 0.0);
	REQUIRE(active);
	REQUIRE(predictive);
	const auto center = active->front();
	CHECK(center.x == -1);
	CHECK(center.y == 0);
	for (size_t i = 0; i < predictive->size(); ++i) {
		const auto& tile = (*predictive)[i];
		const auto dx = std::abs(tile.x - center.x);
		const auto dy = std::abs(tile.y - center.y);
		CHECK(std::max(dx, dy) == 2);
		CHECK((dx == 2 && dy == 2) == (i >= DX12::kPredictiveSideCount));
		CHECK_FALSE(std::ranges::contains(*active, tile));
		CHECK(std::ranges::count(*predictive, tile) == 1);
	}
}

TEST_CASE("DX12 stochastic shadows sample a bounded finite emitter", "[dx12][shadow]")
{
	using namespace DX12Native;
	CHECK(NativeShadowEmitterRadius(400.0f, 0.0f, 0.0f, 100.0f) == 0.0f);
	CHECK(NativeShadowEmitterRadius(400.0f, 0.0f, 0.2f, 100.0f) == 2.0f);
	CHECK(NativeShadowEmitterRadius(1.0f, 0.0f, 1.0f, 100.0f) == 0.5f);
	CHECK(NativeShadowEmitterRadius(400.0f, 4.0f, 1.0f, 100.0f) == 4.0f);
	CHECK(NativeShadowEmitterRadius(400.0f, 0.0f, 8.0f, 4.0f) == 2.0f);

	const auto seed = NativeShadowEmitterSeed(12, 34, 56, 0x123456u);
	CHECK(seed == NativeShadowEmitterSeed(12, 34, 56, 0x123456u));
	CHECK(seed != NativeShadowEmitterSeed(13, 34, 56, 0x123456u));
	CHECK(seed != NativeShadowEmitterSeed(12, 34, 57, 0x123456u));
	CHECK(seed != NativeShadowEmitterSeed(12, 34, 56, 0x123457u));
	for (uint32_t frame = 0; frame < 1024; ++frame) {
		const auto sampleSeed = NativeShadowEmitterSeed(12, 34, frame, 0x123456u);
		for (auto salt : { ShadowEmitterRadialSalt, ShadowEmitterAngularSalt }) {
			const float sample = NativeShadowEmitterRandom(sampleSeed, salt);
			CHECK(sample > 0.0f);
			CHECK(sample < 1.0f);
		}
	}
}

TEST_CASE("DX12 caster removal requires matching geometry and pose", "[dx12][shadow]")
{
	struct Position { float x, y, z; };
	struct Shape {
		std::vector<Position> positions;
		std::vector<uint32_t> indices;
		std::array<float, 12> transform{};
	};
	Shape shape{ { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }, { 0, 1, 2 } };
	std::array<Position, 3> vertices{ Position{ 0, 0, 0 }, Position{ 1, 0, 0 }, Position{ 0, 1, 0 } };
	std::array<uint16_t, 3> indices{ 0, 1, 2 };
	auto transform = shape.transform;
	const auto matches = [&] {
		return DX12Native::MatchesCasterGeometry(shape, transform,
			reinterpret_cast<const uint8_t*>(vertices.data()), indices.data(), sizeof(Position), 3, 1);
	};
	CHECK(matches());
	transform[3] = 1.0f;
	CHECK_FALSE(matches());
	transform = shape.transform;
	vertices[1].x = 2.0f;
	CHECK_FALSE(matches());
	vertices[1].x = 1.0f;
	indices[1] = 2;
	CHECK_FALSE(matches());
	CHECK(DX12Native::CasterCoverageKey(3, 3) != DX12Native::CasterCoverageKey(4, 3));
	CHECK(DX12Native::CasterCoverageKey(0, 3) == 0);
}
