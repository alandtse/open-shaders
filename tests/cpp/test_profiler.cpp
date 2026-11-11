// Runs the production Profiler (src/Profiler.cpp) against a link-time fake of
// Util::TimestampQueryBatch, so no D3D device is needed.
#include "Profiler.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
	struct FakeInterval
	{
		uint64_t begin = 0;
		uint64_t end = 0;
		bool closed = false;
	};

	struct FakeBatch
	{
		uint32_t capacity = 0;
		std::vector<FakeInterval> intervals;
	};

	struct FakeGpu
	{
		static inline std::unordered_map<const void*, FakeBatch> batches;
		static inline uint64_t ticks = 0;
		static inline uint64_t writes = 0;
		static inline uint64_t beginBatchCalls = 0;
		static inline bool beginBatchFails = false;
		static inline Util::TimestampQueryBatch::Status status = Util::TimestampQueryBatch::Status::Ok;

		static void Reset()
		{
			batches.clear();
			ticks = 0;
			writes = 0;
			beginBatchCalls = 0;
			beginBatchFails = false;
			status = Util::TimestampQueryBatch::Status::Ok;
		}
	};

	using Mode = Profiler::CaptureMode;
	using Status = Util::TimestampQueryBatch::Status;

	int deviceToken;
	int contextToken;
	auto* const fakeDevice = reinterpret_cast<ID3D11Device*>(&deviceToken);
	auto* const fakeContext = reinterpret_cast<ID3D11DeviceContext*>(&contextToken);

	void Pass(Profiler& profiler, std::string_view name)
	{
		if (profiler.BeginPass(name, false))
			profiler.EndPass(false);
	}

	const Profiler::TimerResult* Find(const Profiler& profiler, std::string_view name)
	{
		for (const auto& result : profiler.GetResults()) {
			if (result.name == name)
				return &result;
		}
		return nullptr;
	}

	float TopLevelTotal(const Profiler& profiler)
	{
		float total = 0;
		for (const auto& result : profiler.GetResults())
			total += result.topLevelMs;
		return total;
	}

	struct ProfilerFixture
	{
		ProfilerFixture() { FakeGpu::Reset(); }
	};
}

namespace Util
{
	void TimestampQueryBatch::Configure(uint32_t a_maxPairs, const char*)
	{
		FakeGpu::batches[this].capacity = a_maxPairs;
	}

	bool TimestampQueryBatch::Preallocate(ID3D11Device*) { return true; }

	void TimestampQueryBatch::ReleaseQueries() { FakeGpu::batches[this].intervals.clear(); }

	void TimestampQueryBatch::Reset() { FakeGpu::batches[this].intervals.clear(); }

	bool TimestampQueryBatch::BeginBatch(ID3D11Device*, ID3D11DeviceContext*)
	{
		++FakeGpu::beginBatchCalls;
		return !FakeGpu::beginBatchFails;
	}

	void TimestampQueryBatch::EndBatch(ID3D11DeviceContext*) {}

	int TimestampQueryBatch::BeginInterval(ID3D11Device*, ID3D11DeviceContext*) { return -1; }

	void TimestampQueryBatch::CommitInterval(ID3D11DeviceContext*) {}

	int TimestampQueryBatch::AcquireInterval(ID3D11Device*, ID3D11DeviceContext*)
	{
		auto& batch = FakeGpu::batches[this];
		if (batch.intervals.size() >= batch.capacity)
			return -1;
		batch.intervals.push_back({ ++FakeGpu::ticks, 0, false });
		++FakeGpu::writes;
		return static_cast<int>(batch.intervals.size() - 1);
	}

	void TimestampQueryBatch::CloseInterval(ID3D11DeviceContext*, uint32_t a_index)
	{
		auto& interval = FakeGpu::batches[this].intervals.at(a_index);
		interval.end = ++FakeGpu::ticks;
		interval.closed = true;
		++FakeGpu::writes;
	}

	TimestampQueryBatch::Status TimestampQueryBatch::TryResolve(ID3D11DeviceContext*,
		const std::function<void(uint32_t, uint64_t, uint64_t)>& a_visit) const
	{
		if (FakeGpu::status != Status::Ok)
			return FakeGpu::status;
		const auto found = FakeGpu::batches.find(this);
		if (found == FakeGpu::batches.end())
			return Status::Ok;
		const auto& intervals = found->second.intervals;
		for (uint32_t index = 0; index < intervals.size(); ++index) {
			if (intervals[index].closed)
				a_visit(index, intervals[index].end - intervals[index].begin, 1000);
		}
		return Status::Ok;
	}
}

TEST_CASE_METHOD(ProfilerFixture, "Capture modes and filters gate CPU and GPU timing", "[profiler]")
{
	Profiler profiler;
	profiler.Initialize(fakeDevice, fakeContext);

	Pass(profiler, "Idle::Pass");
	CHECK(profiler.GetResults().empty());
	CHECK(FakeGpu::writes == 0);
	CHECK(FakeGpu::beginBatchCalls == 0);

	profiler.RequestCapture(Mode::CPU, "First::");
	profiler.EndFrame(0);
	Pass(profiler, "Filtered::Pass");
	Pass(profiler, "First::Pass");
	profiler.RequestCapture(Mode::GPU, "First::");
	profiler.EndFrame(1);
	CHECK(FakeGpu::writes == 0);
	CHECK(FakeGpu::beginBatchCalls == 0);
	CHECK(profiler.GetCapturedCpuFrameCount() == 1);
	CHECK(Find(profiler, "Filtered::Pass") == nullptr);
	REQUIRE(Find(profiler, "First::Pass") != nullptr);

	Pass(profiler, "First::Pass");
	CHECK(FakeGpu::writes == 2);
	CHECK(profiler.GetCapturedCpuFrameCount() == 1);

	profiler.RequestCapture(Mode::GPU, "First::");
	profiler.RequestCapture(Mode::CPU, "Second::");
	profiler.EndFrame(2);
	FakeGpu::status = Status::NotReady;
	for (uint32_t frame = 3; frame <= 10; ++frame) {
		Pass(profiler, "First::Pass");
		Pass(profiler, "Second::Pass");
		profiler.RequestCapture();
		profiler.EndFrame(frame);
	}
	CHECK(profiler.GetCapturedCpuFrameCount() == 10);
	REQUIRE(Find(profiler, "First::Pass") != nullptr);
	REQUIRE(Find(profiler, "Second::Pass") != nullptr);
	CHECK(Find(profiler, "First::Pass")->activeCpu);
	CHECK(Find(profiler, "Second::Pass")->activeCpu);
	CHECK(profiler.GetCapturedGpuFrameCount() == 0);
}

TEST_CASE_METHOD(ProfilerFixture, "A partial GPU ring drains after a mode switch", "[profiler]")
{
	for (uint32_t capturedFrames : { 1u, 2u }) {
		for (int nextMode = 0; nextMode < 3; ++nextMode) {
			FakeGpu::Reset();
			Profiler profiler;
			profiler.Initialize(fakeDevice, fakeContext);
			profiler.RequestCapture(Mode::GPU);
			profiler.EndFrame(0);
			for (uint32_t frame = 1; frame <= capturedFrames; ++frame) {
				Pass(profiler, "GPU::Pass");
				profiler.RequestCapture(frame < capturedFrames ? Mode::GPU : (nextMode == 0 ? Mode::CPU : Mode::None));
				profiler.EndFrame(frame);
			}
			if (nextMode == 2)
				profiler.SetUserEnabled(false);
			const auto previousWrites = FakeGpu::writes;
			const auto previousBatches = FakeGpu::beginBatchCalls;
			for (uint32_t frame = capturedFrames + 1; frame <= 12; ++frame) {
				Pass(profiler, "CPU::Pass");
				if (nextMode == 0)
					profiler.RequestCapture(Mode::CPU);
				profiler.EndFrame(frame);
			}
			CHECK(profiler.GetCapturedGpuFrameCount() == capturedFrames);
			CHECK(profiler.GetResolvedTotalTimeMs() == TopLevelTotal(profiler));
			CHECK(FakeGpu::writes == previousWrites);
			CHECK(FakeGpu::beginBatchCalls == previousBatches);
			CHECK(profiler.GetTotalTimeMs() == 0);

			profiler.SetUserEnabled(true);
			profiler.RequestCapture(Mode::GPU);
			profiler.EndFrame(13);
			Pass(profiler, "Resumed::Pass");
			profiler.EndFrame(14);
			for (uint32_t frame = 15; frame <= 20; ++frame)
				profiler.EndFrame(frame);
			CHECK(profiler.GetCapturedGpuFrameCount() == 14);
			REQUIRE(Find(profiler, "Resumed::Pass") != nullptr);
			CHECK(Find(profiler, "Resumed::Pass")->activeGpu);
		}
	}
}

TEST_CASE_METHOD(ProfilerFixture, "Retirement keeps each published source", "[profiler]")
{
	for (bool startOnGpu : { false, true }) {
		FakeGpu::Reset();
		Profiler profiler;
		profiler.Initialize(fakeDevice, fakeContext);
		const Mode firstMode = startOnGpu ? Mode::GPU : Mode::CPU;
		const Mode secondMode = startOnGpu ? Mode::CPU : Mode::GPU;
		profiler.RequestCapture(firstMode, "First::");
		profiler.EndFrame(0);
		for (uint32_t frame = 1; frame <= 4; ++frame) {
			Pass(profiler, "First::Pass");
			profiler.RequestCapture(frame == 4 ? secondMode : firstMode, frame == 4 ? "Second::" : "First::");
			profiler.EndFrame(frame);
		}
		for (uint32_t frame = 5; frame <= 75; ++frame) {
			Pass(profiler, "Second::Pass");
			profiler.RequestCapture(secondMode, "Second::");
			profiler.EndFrame(frame);
		}
		const auto* first = Find(profiler, "First::Pass");
		REQUIRE(first != nullptr);
		CHECK((startOnGpu ? first->activeGpu : first->activeCpu));
		CHECK(profiler.GetResolvedTotalTimeMs() == TopLevelTotal(profiler));
		if (!startOnGpu)
			CHECK(profiler.GetResolvedCpuTotalTimeMs() == first->cpuTimeMs);

		profiler.RequestCapture();
		profiler.EndFrame(76);
		for (uint32_t frame = 77; frame <= 145; ++frame) {
			Pass(profiler, "Second::Pass");
			profiler.RequestCapture();
			profiler.EndFrame(frame);
		}
		CHECK(Find(profiler, "First::Pass") == nullptr);
	}
}

TEST_CASE_METHOD(ProfilerFixture, "Nested, repeated, empty and over-capacity passes publish correctly", "[profiler]")
{
	Profiler profiler;
	profiler.Initialize(fakeDevice, fakeContext);
	profiler.RequestCapture();
	profiler.EndFrame(0);
	for (uint32_t frame = 1; frame <= 4; ++frame) {
		REQUIRE(profiler.BeginPass("Parent::Pass", false));
		Pass(profiler, "Child::Pass");
		Pass(profiler, "Child::Pass");
		profiler.EndPass(false);
		profiler.RequestCapture();
		profiler.EndFrame(frame);
	}
	const auto* child = Find(profiler, "Child::Pass");
	REQUIRE(child != nullptr);
	CHECK(child->historyCount == 1);
	CHECK(child->cpuHistoryCount == 4);
	CHECK(TopLevelTotal(profiler) == profiler.GetResolvedTotalTimeMs());

	float selfTotal = 0;
	for (const auto& result : profiler.GetResults())
		selfTotal += result.gpuTimeMs;
	CHECK(selfTotal == TopLevelTotal(profiler));

	for (uint32_t index = 0; index < Profiler::kMaxTimers; ++index)
		Pass(profiler, "Capacity::Pass");
	Pass(profiler, "Overflow::Pass");
	profiler.RequestCapture(Mode::GPU, "Missing::");
	profiler.EndFrame(5);
	CHECK(profiler.GetSlotRefusals() == 1);
	REQUIRE(Find(profiler, "Overflow::Pass") != nullptr);
	CHECK(Find(profiler, "Overflow::Pass")->activeCpu);

	for (uint32_t frame = 6; frame <= 12; ++frame) {
		Pass(profiler, "Filtered::Pass");
		profiler.RequestCapture(Mode::GPU, "Missing::");
		profiler.EndFrame(frame);
	}
	CHECK(profiler.GetAcquiredSlots() == 0);
	CHECK(profiler.GetResolvedTotalTimeMs() == 0);

	profiler.ClearTimers();
	for (uint32_t frame = 13; frame <= 17; ++frame)
		profiler.EndFrame(frame);
	CHECK(profiler.GetResults().empty());

	profiler.Initialize(fakeDevice, fakeContext);
	CHECK(profiler.GetResults().empty());
	CHECK(profiler.GetCapturedCpuFrameCount() == 0);
	CHECK(profiler.GetCapturedGpuFrameCount() == 0);
}

TEST_CASE_METHOD(ProfilerFixture, "A frame refused at its first pass is skipped whole", "[profiler]")
{
	for (bool batchFails : { false, true }) {
		FakeGpu::Reset();
		Profiler profiler;
		profiler.Initialize(fakeDevice, fakeContext);
		profiler.RequestCapture(Mode::GPU);
		profiler.EndFrame(0);
		for (uint32_t frame = 1; frame <= 7; ++frame) {
			if (frame == 4) {
				if (batchFails)
					FakeGpu::beginBatchFails = true;
				else
					FakeGpu::status = Status::NotReady;
			}
			Pass(profiler, "Early::Pass");
			FakeGpu::beginBatchFails = false;
			FakeGpu::status = Status::Ok;
			Pass(profiler, "Late::Pass");
			if (frame < 7)
				profiler.RequestCapture(Mode::GPU);
			profiler.EndFrame(frame);
		}
		for (uint32_t frame = 8; frame <= 14; ++frame)
			profiler.EndFrame(frame);

		const auto* early = Find(profiler, "Early::Pass");
		const auto* late = Find(profiler, "Late::Pass");
		REQUIRE(early != nullptr);
		REQUIRE(late != nullptr);
		CHECK(early->historyCount == 6);
		CHECK(late->historyCount == 6);
		for (uint32_t index = 0; index < early->historyCount; ++index)
			CHECK(early->GetHistorySample(index) > 0);
		CHECK(profiler.GetCapturedGpuFrameCount() == 7);
	}
}
