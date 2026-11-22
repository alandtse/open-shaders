#include "DX12NativeAcceleration.h"

#include <chrono>
#include <limits>
#include <utility>

namespace DX12Native
{
	namespace
	{
		constexpr auto kGpuRetirePoll = std::chrono::milliseconds(8);

		struct RetainedScene
		{
			std::shared_ptr<const TopLevel> scene;
			winrt::com_ptr<ID3D12Fence> fence;
			uint64_t value = 0;
		};
	}

	struct AccelerationWorker::State
	{
		std::mutex mutex;
		std::condition_variable_any wake;
		std::shared_ptr<const DX12::Grid> pending;
		std::vector<RetainedScene> consumers;
		std::atomic<std::shared_ptr<const TopLevel>> published{};
		uint64_t epoch = 0;
		uint64_t lastRevision = std::numeric_limits<uint64_t>::max();
		uint32_t lastScope = 0;
		std::string failure;
	};

	AccelerationWorker::AccelerationWorker() : state(std::make_shared<State>()) {}

	AccelerationWorker::~AccelerationWorker()
	{
		if (!worker.joinable())
			return;
		worker.request_stop();
		state->wake.notify_all();
		worker.join();
	}

	bool AccelerationWorker::Start(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		if (!a_device || !a_context)
			return false;
		if (worker.joinable())
			return true;
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;
		device.copy_from(a_device);
		context.copy_from(a_context);
		auto retained = state;
		worker = std::jthread([retained, device = std::move(device),
								  context = std::move(context)](std::stop_token stop) mutable {
			Run(std::move(retained), stop, std::move(device), std::move(context));
		});
		return true;
	}

	void AccelerationWorker::Request(std::shared_ptr<const DX12::Grid> a_grid)
	{
		if (!a_grid || !a_grid->requested.front().scope)
			return;
		const uint32_t scope = a_grid->requested.front().scope;
		{
			std::scoped_lock lock(state->mutex);
			if (state->lastScope == scope && state->lastRevision == a_grid->geometryRevision)
				return;
			if (state->lastScope && state->lastScope != scope)
				state->published.store(nullptr, std::memory_order_release);
			state->lastScope = scope;
			state->lastRevision = a_grid->geometryRevision;
			state->pending = std::move(a_grid);
			++state->epoch;
		}
		state->wake.notify_one();
	}

	void AccelerationWorker::Reset()
	{
		{
			std::scoped_lock lock(state->mutex);
			state->lastScope = 0;
			state->lastRevision = std::numeric_limits<uint64_t>::max();
			state->pending.reset();
			state->published.store(nullptr, std::memory_order_release);
			++state->epoch;
		}
		state->wake.notify_one();
	}

	std::shared_ptr<const TopLevel> AccelerationWorker::GetScene() const
	{
		return state->published.load(std::memory_order_acquire);
	}

	bool AccelerationWorker::RetainThrough(std::shared_ptr<const TopLevel> a_scene,
		ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_scene || !a_fence || !a_value)
			return false;
		RetainedScene record;
		record.scene = std::move(a_scene);
		record.fence.copy_from(a_fence);
		record.value = a_value;
		{
			std::scoped_lock lock(state->mutex);
			state->consumers.push_back(std::move(record));
		}
		state->wake.notify_one();
		return true;
	}

	std::string AccelerationWorker::Failure() const
	{
		std::scoped_lock lock(state->mutex);
		return state->failure;
	}

	void AccelerationWorker::Run(std::shared_ptr<State> a_state, std::stop_token a_stop,
		winrt::com_ptr<ID3D11Device> a_device, winrt::com_ptr<ID3D11DeviceContext> a_context)
	{
		std::string failure;
		auto backend = GraphicsBackend::Create(a_device.get(), a_context.get(), failure);
		AccelerationScene acceleration;
		if (!backend || !acceleration.Initialize(*backend)) {
			std::scoped_lock lock(a_state->mutex);
			a_state->failure = failure.empty() ? acceleration.Failure() : failure;
			if (a_state->failure.empty())
				a_state->failure = "Native acceleration initialization unavailable";
			return;
		}

		std::shared_ptr<const SceneInputs> inputs;
		uint64_t processedEpoch = 0;
		std::vector<RetainedScene> consumers;
		while (!a_stop.stop_requested()) {
			std::shared_ptr<const DX12::Grid> next;
			bool changed = false;
			{
				std::scoped_lock lock(a_state->mutex);
				if (processedEpoch != a_state->epoch) {
					processedEpoch = a_state->epoch;
					next = std::exchange(a_state->pending, nullptr);
					changed = true;
				}
				for (auto& record : a_state->consumers)
					consumers.push_back(std::move(record));
				a_state->consumers.clear();
			}

			if (changed) {
				if (next)
					inputs = CollectSceneInputs(std::move(next));
				else {
					inputs.reset();
					acceleration.Reset();
				}
			}
			acceleration.Advance(inputs);
			const auto completed = acceleration.GetScene();
			if (completed) {
				std::scoped_lock lock(a_state->mutex);
				if (processedEpoch == a_state->epoch &&
					!a_stop.stop_requested())
					a_state->published.store(completed, std::memory_order_release);
			}
			if (!acceleration.Failure().empty()) {
				std::scoped_lock lock(a_state->mutex);
				a_state->failure = acceleration.Failure();
				break;
			}

			std::erase_if(consumers, [](const RetainedScene& record) {
				return record.fence->GetCompletedValue() >= record.value;
			});
			const bool building = inputs && !completed;
			std::unique_lock lock(a_state->mutex);
			auto workArrived = [&] {
				return a_stop.stop_requested() || processedEpoch != a_state->epoch ||
				       !a_state->consumers.empty();
			};
			if (building || !consumers.empty())
				a_state->wake.wait_for(lock, kGpuRetirePoll, workArrived);
			else
				a_state->wake.wait(lock, workArrived);
		}

		// GPU consumer lifetimes continue independently of worker cancellation.
		{
			std::scoped_lock lock(a_state->mutex);
			for (auto& record : a_state->consumers)
				consumers.push_back(std::move(record));
			a_state->consumers.clear();
		}
		if (!consumers.empty()) {
			std::thread([hold = std::move(consumers)]() mutable {
				while (!hold.empty()) {
					std::erase_if(hold, [](const RetainedScene& record) {
						return record.fence->GetCompletedValue() >= record.value;
					});
					if (!hold.empty())
						std::this_thread::sleep_for(kGpuRetirePoll);
				}
			}).detach();
		}
	}
}
