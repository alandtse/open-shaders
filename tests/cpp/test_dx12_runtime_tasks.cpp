#include "Features/DX12/DX12FenceRetirementPolicy.h"
#include "Features/DX12/SpatialGrid.h"
#include "Features/DX12/TilePreparationPolicy.h"
#include "../../package/Shaders/Common/NativeShadowRT.hlsli"
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

TEST_CASE("DX12 empty tile classification distinguishes failure from completed enumeration", "[dx12][resources]")
{
	using DX12::ClassifyTilePreparation;
	using DX12::TilePreparationOutcome;
	CHECK(ClassifyTilePreparation(true, true, true, false) == TilePreparationOutcome::Empty);
	CHECK(ClassifyTilePreparation(true, true, false, false) == TilePreparationOutcome::EmptyPartial);
	CHECK(ClassifyTilePreparation(true, false, false, false) == TilePreparationOutcome::Retry);
	CHECK(ClassifyTilePreparation(false, true, true, false) == TilePreparationOutcome::Retry);
	CHECK(ClassifyTilePreparation(true, true, false, true) == TilePreparationOutcome::Partial);
	CHECK(ClassifyTilePreparation(true, true, true, true) == TilePreparationOutcome::Ready);
}

TEST_CASE("DX12 offloading has one quarter-resolution layout", "[dx12][shadow]")
{
	using namespace DX12Native;
	CHECK(NativeShadowOffloadExtent(1920) == 960);
	CHECK(NativeShadowOffloadExtent(1080) == 540);
	CHECK(NativeShadowOffloadExtent(1921) == 961);
	CHECK(NativeShadowOffloadExtent(1081) == 541);
	CHECK(NativeShadowOffloadExtent(1) == 1);
	CHECK(NativeShadowOffloadRenderCoordinate(0, 1920) == 1);
	CHECK(NativeShadowOffloadRenderCoordinate(959, 1920) == 1919);
	CHECK(NativeShadowOffloadRenderCoordinate(960, 1921) == 1920);
	CHECK(NativeShadowOffloadRenderCoordinate(0, 1) == 0);
	CHECK(kOffloadingBlend == 0.40f);
	CHECK(kOffloadingEmitterMultiplier == 0.30f);
	CHECK(kOffloadingFadeRange == 0.66f);
	CHECK(kOffloadingNearRadius == 16.0f);
}

TEST_CASE("DX12 full RT keeps the two accepted checkerboard layouts", "[dx12][shadow]")
{
	using namespace DX12Native;
	CHECK(SanitizeRTQuality(0) == kShadowQualityQuality);
	CHECK(SanitizeRTQuality(3) == kShadowQualityPerformance);
	CHECK(ShadowPhysicalWidth(ShadowLogicalExtent(1920, kShadowQualityQuality), kShadowQualityQuality) == 960);
	CHECK(ShadowLogicalExtent(1080, kShadowQualityQuality) == 1080);
	CHECK(ShadowPhysicalWidth(ShadowLogicalExtent(1920, kShadowQualityPerformance), kShadowQualityPerformance) == 480);
	CHECK(ShadowLogicalExtent(1080, kShadowQualityPerformance) == 540);
	CHECK(ShadowPhysicalWidth(ShadowLogicalExtent(1921, kShadowQualityQuality), kShadowQualityQuality) == 961);
	CHECK(ShadowPhysicalWidth(ShadowLogicalExtent(1921, kShadowQualityPerformance), kShadowQualityPerformance) == 481);
	CHECK(ShadowLogicalExtent(1081, kShadowQualityPerformance) == 541);
	CHECK(ShadowLogicalX(0, 0, 0) == 0);
	CHECK(ShadowLogicalX(0, 0, 1) == 1);
	CHECK(ShadowLogicalX(0, 1, 0) == 1);
	CHECK(ShadowLogicalX(0, 1, 1) == 0);
	CHECK(ShadowRenderCoordinate(480, 961, kShadowQualityPerformance) == 960);
	CHECK(ShadowEmitterMultiplier(kShadowQualityQuality) == 0.65f);
	CHECK(ShadowEmitterMultiplier(kShadowQualityPerformance) == 0.20f);
	CHECK(ShadowEmitterMultiplier(kShadowQualityOffloading) == 0.30f);
	CHECK(ShadowVariant(kShadowQualityOffloading) == 0);
	CHECK(ShadowVariant(kShadowQualityQuality) == 1);
	CHECK(ShadowVariant(kShadowQualityPerformance) == 2);
}
