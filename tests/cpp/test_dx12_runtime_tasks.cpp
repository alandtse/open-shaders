#include "Features/DX12/DX12FenceRetirementPolicy.h"
#include "Features/DX12/DX12CasterCoverage.h"
#include "Features/DX12/SpatialGrid.h"
#include "Features/DX12/FullShadowLightConsolidation.h"
#include "Features/DX12/TilePreparationPolicy.h"
#include "Utils/RuntimeTaskGate.h"
#include "../../features/DX12 Native Scene/Shaders/DX12NativeScene/NativeShadowSampling.hlsli"
#include "../../package/Shaders/Common/NativeShadowQuality.hlsli"

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

TEST_CASE("DX12 shadow quality allocates packed physical extents", "[dx12][shadow]")
{
	using namespace DX12Native;
	constexpr std::array<uint32_t, kShadowQualityCount> expectedWidths{ 3840, 1920, 1920, 960, 1280 };
	constexpr std::array<uint32_t, kShadowQualityCount> expectedHeights{ 2160, 2160, 1080, 1080, 720 };
	for (uint32_t mode = 0; mode < kShadowQualityCount; ++mode) {
		const auto logicalWidth = NativeShadowLogicalExtent(3840, mode);
		const auto logicalHeight = NativeShadowLogicalExtent(2160, mode);
		CHECK(NativeShadowPhysicalWidth(logicalWidth, mode) == expectedWidths[mode]);
		CHECK(logicalHeight == expectedHeights[mode]);
		for (uint32_t frame = 0; frame < 2; ++frame) {
			const auto origin = NativeShadowCheckerboardOrigin(mode, frame);
			CHECK(origin == (NativeShadowCheckerboard(mode) ? frame : 0u));
			for (uint32_t y = 0; y < 3; ++y) {
				const auto oddWidth = NativeShadowLogicalExtent(13, mode);
				const auto packedWidth = NativeShadowPhysicalWidth(oddWidth, mode);
				for (uint32_t x = 0; x < packedWidth; ++x) {
					const auto logical = NativeShadowLogicalX(x, y, mode, origin);
					if (logical >= oddWidth)
						continue;
					CHECK(NativeShadowHasSample(logical, y, mode, origin));
					CHECK(NativeShadowPackedX(logical, mode) == x);
					CHECK(NativeShadowRenderCoordinate(logical, 13, mode) < 13);
				}
			}
		}
	}
	CHECK(NativeShadowLogicalX(0, 0, kShadowQualityQuality, 0) == 0);
	CHECK(NativeShadowLogicalX(0, 0, kShadowQualityQuality, 1) == 1);
	CHECK(NativeShadowLogicalX(0, 1, kShadowQualityQuality, 0) == 1);
	CHECK(NativeShadowLogicalX(0, 1, kShadowQualityQuality, 1) == 0);
}

namespace
{
	struct ConsolidationLight
	{
		struct Vector { float x, y, z; };
		uint32_t referenceID = 0, flags = 0;
		Vector position{}, color{ 1, 1, 1 };
		float radius = 100, fade = 1, emitterRadius = 0, falloffExponent = 1;
		uint64_t runtimeIdentity = 0;
		uint32_t unifiedCount = 1;
	};
}

TEST_CASE("Full-shadow consolidation preserves prepared authority", "[dx12][shadow]")
{
	using namespace DX12Native;
	using Light = ConsolidationLight;
	const std::vector<Light> prepared{ { .referenceID = 10, .position = { 0, 0, 0 }, .fade = 2 } };
	const std::vector<Light> runtime{
		{ .referenceID = 20, .position = { 128, 0, 0 }, .runtimeIdentity = 1 },
		{ .referenceID = 20, .position = { 132, 0, 0 }, .runtimeIdentity = 2 },
		{ .referenceID = 20, .position = { 136, 0, 0 }, .runtimeIdentity = 3 }
	};
	auto result = ConsolidateFullShadowLights<Light>(prepared, runtime, 64, 128);
	REQUIRE(result.size() == 2);
	CHECK(SamePreparedShadowLight(result[0], prepared[0]));
	CHECK(result[1].unifiedCount == 2);
	CHECK(result[1].color.x * result[1].fade == 2);
	CHECK(result[1].position.x == 134);
	result = ConsolidateFullShadowLights<Light>(prepared, runtime, 0, 0);
	CHECK(result.size() == 4);
	CHECK(SanitizeFullShadowRadius(63) == 64);
	CHECK(SanitizeFullShadowRadius(513) == 512);
}

TEST_CASE("Full-shadow runtime grouping is owner-aware and deterministic", "[dx12][shadow]")
{
	using namespace DX12Native;
	using Light = ConsolidationLight;
	std::vector<Light> runtime{
		{ .referenceID = 7, .position = { -2, 0, 0 }, .color = { 1, 0, 0 }, .radius = 100, .fade = 2, .runtimeIdentity = 1 },
		{ .referenceID = 7, .position = { 2, 0, 0 }, .color = { 0, 1, 0 }, .radius = 200, .fade = 3, .runtimeIdentity = 2 },
		{ .referenceID = 7, .position = { 100, 0, 0 }, .runtimeIdentity = 3 },
		{ .referenceID = 8, .position = { 0, 0, 0 }, .runtimeIdentity = 4 },
		{ .referenceID = 0, .position = { 0, 0, 0 }, .runtimeIdentity = 5 },
		{ .referenceID = 0, .position = { 0, 0, 0 }, .runtimeIdentity = 6 }
	};
	const auto result = ConsolidateFullShadowLights<Light>({}, runtime, 64, 0);
	REQUIRE(result.size() == 5);
	CHECK(result[0].unifiedCount == 2);
	CHECK(result[0].color.x == 2);
	CHECK(result[0].color.y == 3);
	CHECK(result[0].color.z == 0);
	CHECK(result[0].fade == 1);
	CHECK(result[0].radius == 200);
	CHECK(result[0].position.x > 0);
	CHECK(result[0].position.x < 2);
	std::ranges::reverse(runtime);
	const auto reordered = ConsolidateFullShadowLights<Light>({}, runtime, 64, 0);
	REQUIRE(reordered.size() == result.size());
	for (size_t i = 0; i < result.size(); ++i) {
		CHECK(SamePreparedShadowLight(result[i], reordered[i]));
		CHECK(result[i].runtimeIdentity == reordered[i].runtimeIdentity);
	}
}

TEST_CASE("Four equal brazier emitters yield four times the native emission", "[dx12][shadow]")
{
	using namespace DX12Native;
	using Light = ConsolidationLight;
	const std::vector<Light> runtime{
		{ .referenceID = 7, .position = { -12, 0, 0 }, .runtimeIdentity = 1 },
		{ .referenceID = 7, .position = { 12, 0, 0 }, .runtimeIdentity = 2 },
		{ .referenceID = 7, .position = { 0, -12, 0 }, .runtimeIdentity = 3 },
		{ .referenceID = 7, .position = { 0, 12, 0 }, .runtimeIdentity = 4 }
	};
	const auto result = ConsolidateFullShadowLights<Light>({}, runtime, 64, 0);
	REQUIRE(result.size() == 1);
	CHECK(result.front().unifiedCount == 4);
	CHECK(result.front().color.x * result.front().fade == 4);
	CHECK(result.front().position.x == 0);
	CHECK(result.front().position.y == 0);
	const std::vector<Light> prepared{ { .referenceID = 9, .position = { 0, 0, 0 } } };
	const auto excluded = ConsolidateFullShadowLights<Light>(prepared, runtime, 64, 128);
	REQUIRE(excluded.size() == 1);
	CHECK(SamePreparedShadowLight(excluded.front(), prepared.front()));
	CHECK(excluded.front().unifiedCount == 1);
}

TEST_CASE("Prepared emission addition is opt-in and assigned once", "[dx12][shadow]")
{
	using namespace DX12Native;
	using Light = ConsolidationLight;
	const std::vector<Light> prepared{
		{ .referenceID = 1, .position = { 0, 0, 0 }, .color = { 1, 2, 3 }, .radius = 300, .fade = 2 },
		{ .referenceID = 2, .position = { 100, 0, 0 }, .fade = 3 }
	};
	const std::vector<Light> runtime{
		{ .referenceID = 7, .position = { 10, 0, 0 }, .color = { 3, 1, 0 }, .fade = 2, .runtimeIdentity = 1 },
		{ .referenceID = 8, .position = { 50, 0, 0 }, .runtimeIdentity = 2 },
		{ .referenceID = 9, .position = { 99, 0, 0 }, .runtimeIdentity = 3 }
	};
	const auto disabled = ConsolidateFullShadowLights<Light>(prepared, runtime, 128, 256, false);
	REQUIRE(disabled.size() == 2);
	CHECK(SamePreparedShadowLight(disabled[0], prepared[0]));
	CHECK(SamePreparedShadowLight(disabled[1], prepared[1]));
	const auto enabled = ConsolidateFullShadowLights<Light>(prepared, runtime, 128, 256, true);
	REQUIRE(enabled.size() == 2);
	CHECK(enabled[0].color.x * enabled[0].fade == 9);
	CHECK(enabled[0].color.y * enabled[0].fade == 7);
	CHECK(enabled[0].color.z * enabled[0].fade == 7);
	CHECK(enabled[1].color.x * enabled[1].fade == 4);
	CHECK(enabled[0].position.x == prepared[0].position.x);
	CHECK(enabled[0].radius == prepared[0].radius);
	CHECK(enabled[0].referenceID == prepared[0].referenceID);
	CHECK(ConsolidateFullShadowLights<Light>(prepared, runtime, 0, 0, true).size() == 5);
}
