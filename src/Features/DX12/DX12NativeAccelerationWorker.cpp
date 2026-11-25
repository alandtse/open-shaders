#include "DX12NativeAcceleration.h"

#include "DX12GpuRetirement.h"
#include "Globals.h"
#include "ShaderCache.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace DX12Native
{
	namespace
	{
		struct RetainedScene
		{
			std::shared_ptr<const TopLevel> scene;
			winrt::com_ptr<ID3D12Fence> fence;
			uint64_t value = 0;
		};

		bool IsRetired(const RetainedScene& a_record)
		{
			return GpuFenceRetired(a_record.fence.get(), a_record.value);
		}
	}

	struct AccelerationWorker::State
	{
		~State()
		{
			// State is kept alive by the admitted task. Existing GPU retirement
			// ownership remains independent of the shared CPU task lifetime.
			std::erase_if(consumers, IsRetired);
			if (!consumers.empty()) {
				winrt::com_ptr<ID3D12Device> device;
				(void)consumers.front().fence->GetDevice(IID_PPV_ARGS(device.put()));
				std::vector<FenceDependency> dependencies;
				for (const auto& record : consumers)
					dependencies.push_back({ record.fence, record.value });
				RetireAfterFences(std::move(consumers), std::move(device), std::move(dependencies));
			}
		}

		std::mutex mutex;
		std::shared_ptr<const DX12::Grid> pending;
		std::vector<RetainedScene> consumers;
		std::atomic<std::shared_ptr<const TopLevel>> published{};
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;
		uint64_t epoch = 0;
		uint64_t resetSerial = 0;
		uint64_t lastRevision = std::numeric_limits<uint64_t>::max();
		uint32_t lastScope = 0;
		bool scheduled = false;
		bool closing = false;
		bool needsAdvance = false;
		winrt::com_ptr<ID3D12Fence> waitFence;
		uint64_t waitValue = 0;
		std::string failure;

		// Accessed only by the single admitted task, never the render thread.
		std::unique_ptr<AccelerationScene> acceleration;
		std::shared_ptr<const SceneInputs> inputs;
		uint64_t processedEpoch = 0;
		uint64_t processedResetSerial = 0;
	};

	AccelerationWorker::AccelerationWorker() = default;

	AccelerationWorker::~AccelerationWorker()
	{
		Stop();
	}

	bool AccelerationWorker::Start(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		if (!a_device || !a_context)
			return false;
		if (!state.load(std::memory_order_acquire)) {
			auto next = std::make_shared<State>();
			next->device.copy_from(a_device);
			next->context.copy_from(a_context);
			state.store(std::move(next), std::memory_order_release);
		}
		return true;
	}

	void AccelerationWorker::Request(std::shared_ptr<const DX12::Grid> a_grid)
	{
		auto current = state.load(std::memory_order_acquire);
		if (!current || !a_grid || !a_grid->requested.front().scope)
			return;
		const uint32_t scope = a_grid->requested.front().scope;
		{
			std::scoped_lock lock(current->mutex);
			if (current->closing)
				return;
			if (current->lastScope != scope || current->lastRevision != a_grid->geometryRevision) {
				if (current->lastScope && current->lastScope != scope)
					current->published.store(nullptr, std::memory_order_release);
				current->lastScope = scope;
				current->lastRevision = a_grid->geometryRevision;
				current->pending = std::move(a_grid);
				++current->epoch;
			}
		}
		TrySchedule(current);
	}

	void AccelerationWorker::Reset()
	{
		auto current = state.load(std::memory_order_acquire);
		if (!current)
			return;
		{
			std::scoped_lock lock(current->mutex);
			current->lastScope = 0;
			current->lastRevision = std::numeric_limits<uint64_t>::max();
			current->pending.reset();
			current->published.store(nullptr, std::memory_order_release);
			current->failure.clear();
			++current->resetSerial;
			++current->epoch;
		}
		TrySchedule(current);
	}

	void AccelerationWorker::Stop()
	{
		auto current = state.exchange(nullptr, std::memory_order_acq_rel);
		if (!current)
			return;
		std::scoped_lock lock(current->mutex);
		current->closing = true;
		current->pending.reset();
		current->published.store(nullptr, std::memory_order_release);
		++current->epoch;
	}

	std::shared_ptr<const TopLevel> AccelerationWorker::GetScene() const
	{
		auto current = state.load(std::memory_order_acquire);
		return current ? current->published.load(std::memory_order_acquire) : nullptr;
	}

	bool AccelerationWorker::RetainThrough(std::shared_ptr<const TopLevel> a_scene,
		ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_scene || !a_fence || !a_value)
			return false;
		auto current = state.load(std::memory_order_acquire);
		if (!current)
			return false;
		RetainedScene record;
		record.scene = std::move(a_scene);
		record.fence.copy_from(a_fence);
		record.value = a_value;
		{
			std::scoped_lock lock(current->mutex);
			if (current->closing)
				return false;
			current->consumers.push_back(std::move(record));
		}
		TrySchedule(current);
		return true;
	}

	std::string AccelerationWorker::Failure() const
	{
		auto current = state.load(std::memory_order_acquire);
		if (!current)
			return {};
		std::scoped_lock lock(current->mutex);
		return current->failure;
	}

	void AccelerationWorker::TrySchedule(const std::shared_ptr<State>& a_state)
	{
		std::scoped_lock lock(a_state->mutex);
		if (a_state->closing || a_state->scheduled || !a_state->failure.empty() || !globals::shaderCache)
			return;

		// Active frame callbacks retry admission and check fence readiness. Never
		// occupy a pool slot while a fence is pending; no timer or dedicated waiter.
		const bool changed = a_state->epoch != a_state->processedEpoch;
		const bool ready = a_state->needsAdvance &&
		                   (!a_state->waitFence || GpuFenceRetired(a_state->waitFence.get(), a_state->waitValue));
		const bool retire = std::ranges::any_of(a_state->consumers, IsRetired);
		if (!changed && !ready && !retire)
			return;

		a_state->scheduled = true;
		if (!globals::shaderCache->TrySubmitRuntimeTask([retained = a_state] { RunStep(retained); }))
			a_state->scheduled = false;
	}

	void AccelerationWorker::RunStep(std::shared_ptr<State> a_state)
	{
		uint64_t epoch = 0;
		uint64_t resetSerial = 0;
		bool changed = false;
		bool reset = false;
		std::shared_ptr<const DX12::Grid> next;
		std::shared_ptr<const TopLevel> completed;
		winrt::com_ptr<ID3D12Fence> waitFence;
		uint64_t waitValue = 0;
		bool needsAdvance = false;
		std::string failure;

		try {
			{
				std::scoped_lock lock(a_state->mutex);
				if (a_state->closing) {
					a_state->scheduled = false;
					return;
				}
				epoch = a_state->epoch;
				resetSerial = a_state->resetSerial;
				changed = epoch != a_state->processedEpoch;
				reset = resetSerial != a_state->processedResetSerial;
				if (changed)
					next = std::exchange(a_state->pending, nullptr);
				std::erase_if(a_state->consumers, IsRetired);
			}

			if (reset) {
				a_state->inputs.reset();
				if (a_state->acceleration && !a_state->acceleration->Failure().empty())
					a_state->acceleration.reset();
				else if (a_state->acceleration)
					a_state->acceleration->Reset();
			}
			if (changed)
				a_state->inputs = next ? CollectSceneInputs(std::move(next)) : nullptr;

			if (!a_state->acceleration && a_state->inputs) {
				auto backend = GraphicsBackend::Create(a_state->device.get(), a_state->context.get(), failure);
				auto acceleration = std::make_unique<AccelerationScene>();
				if (!backend || !acceleration->Initialize(*backend)) {
					if (failure.empty())
						failure = acceleration->Failure();
					if (failure.empty())
						failure = "Native acceleration initialization unavailable";
				} else {
					a_state->acceleration = std::move(acceleration);
				}
			}
			if (a_state->acceleration && failure.empty()) {
				auto& acceleration = *a_state->acceleration;
				acceleration.Advance(a_state->inputs);
				completed = acceleration.GetScene();
				failure = acceleration.Failure();
				needsAdvance = (a_state->inputs && !completed) || acceleration.HasPendingBuilds();
				for (const auto& build : acceleration.builds) {
					if (build.fence && build.fence != UINT64_MAX && (!waitValue || build.fence < waitValue))
						waitValue = build.fence;
				}
				if (waitValue)
					waitFence = acceleration.completion;
			}
		} catch (const std::exception& error) {
			failure = error.what();
		} catch (...) {
			failure = "Unknown native acceleration task failure";
		}

		std::scoped_lock lock(a_state->mutex);
		a_state->processedEpoch = epoch;
		a_state->processedResetSerial = resetSerial;
		a_state->needsAdvance = needsAdvance;
		a_state->waitFence = std::move(waitFence);
		a_state->waitValue = waitValue;
		if (!a_state->closing && epoch == a_state->epoch) {
			if (!failure.empty()) {
				a_state->failure = std::move(failure);
				a_state->published.store(nullptr, std::memory_order_release);
				logger::error("DX12 acceleration task failed: {}", a_state->failure);
			} else if (completed) {
				a_state->published.store(std::move(completed), std::memory_order_release);
			}
		}
		a_state->scheduled = false;
	}
}
