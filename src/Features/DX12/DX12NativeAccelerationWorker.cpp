#include "DX12NativeAcceleration.h"

#include "DX12GpuRetirement.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "Utils/TracyZones.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace DX12Native
{
	struct AccelerationWorker::State
	{
		std::mutex mutex;
		std::shared_ptr<const DX12::Grid> pending;
		std::shared_ptr<Util::RuntimeTaskGate> taskGate;
		std::atomic<std::shared_ptr<const TopLevel>> published{};
		std::atomic<uint64_t> budgetDeferredRevision{ UINT64_MAX };
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;
		uint64_t epoch = 0;
		uint64_t resetSerial = 0;
		uint64_t lastRevision = std::numeric_limits<uint64_t>::max();
		uint32_t lastScope = 0;
		bool scheduled = false;
		bool closing = false;
		bool needsAdvance = false;
		bool menuPriority = false;
		winrt::com_ptr<ID3D12Fence> waitFence;
		uint64_t waitValue = 0;
		bool waitRegistered = false;
		bool waitReady = false;
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
			if (!globals::shaderCache)
				return false;
			auto gate = globals::shaderCache->GetRuntimeTaskGate();
			if (!gate)
				return false;
			auto next = std::make_shared<State>();
			next->taskGate = std::move(gate);
			next->device.copy_from(a_device);
			next->context.copy_from(a_context);
			state.store(std::move(next), std::memory_order_release);
		}
		return true;
	}

	void AccelerationWorker::Request(std::shared_ptr<const DX12::Grid> a_grid, bool menuPriority)
	{
		auto current = state.load(std::memory_order_acquire);
		if (!current || !a_grid || !a_grid->requested.front().scope)
			return;
		const uint32_t scope = a_grid->requested.front().scope;
		{
			std::scoped_lock lock(current->mutex);
			if (current->closing)
				return;
			current->menuPriority = menuPriority;
			if (current->lastScope != scope || current->lastRevision != a_grid->geometryRevision) {
				if (current->lastScope && current->lastScope != scope)
					current->published.store(nullptr, std::memory_order_release);
				current->lastScope = scope;
				current->lastRevision = a_grid->geometryRevision;
				current->budgetDeferredRevision.store(UINT64_MAX, std::memory_order_release);
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
			current->menuPriority = false;
			current->budgetDeferredRevision.store(UINT64_MAX, std::memory_order_release);
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

	bool AccelerationWorker::WholeCellBudgetDeferred(uint64_t revision) const
	{
		const auto current = state.load(std::memory_order_acquire);
		return current && current->budgetDeferredRevision.load(std::memory_order_acquire) == revision;
	}

	void AccelerationWorker::TrySchedule(const std::shared_ptr<State>& a_state)
	{
		std::scoped_lock lock(a_state->mutex);
		if (a_state->closing || a_state->scheduled || !a_state->failure.empty() || !a_state->taskGate)
			return;

		// A fence callback marks readiness and attempts admission. Active frames
		// retry denied admission and are a nonblocking fallback if registration fails.
		const bool changed = a_state->epoch != a_state->processedEpoch;
		const bool ready = a_state->needsAdvance &&
		                   (!a_state->waitFence || a_state->waitReady ||
							   (!a_state->waitRegistered && GpuFenceRetired(a_state->waitFence.get(), a_state->waitValue)));
		if (!changed && !ready)
			return;

		a_state->scheduled = true;
		if (!a_state->taskGate->TrySubmit([retained = a_state] { RunStep(retained); }, a_state->menuPriority))
			a_state->scheduled = false;
	}

	void AccelerationWorker::RunStep(std::shared_ptr<State> a_state)
	{
		ZoneScopedN("DX12::Acceleration::Task");
		uint64_t epoch = 0;
		uint64_t resetSerial = 0;
		bool changed = false;
		bool reset = false;
		std::shared_ptr<const DX12::Grid> next;
		std::shared_ptr<const TopLevel> completed;
		winrt::com_ptr<ID3D12Fence> waitFence;
		uint64_t waitValue = 0;
		bool needsAdvance = false;
		uint64_t deferredRevision = UINT64_MAX;
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
			}

			if (reset) {
				a_state->inputs.reset();
				if (a_state->acceleration && !a_state->acceleration->Failure().empty())
					a_state->acceleration.reset();
				else if (a_state->acceleration)
					a_state->acceleration->Reset();
			}
			if (changed) {
				ZoneScopedN("DX12::Acceleration::CollectInputs");
				a_state->inputs = next ? CollectSceneInputs(std::move(next)) : nullptr;
			}

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
				if (acceleration.BudgetDeferred() && a_state->inputs && a_state->inputs->residency && a_state->inputs->residency->wholeCell)
					deferredRevision = a_state->inputs->geometryRevision;
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
		const bool newWait = a_state->waitFence.get() != waitFence.get() || a_state->waitValue != waitValue;
		a_state->waitFence = std::move(waitFence);
		a_state->waitValue = waitValue;
		if (newWait) {
			a_state->waitRegistered = false;
			a_state->waitReady = false;
		}
		if (!a_state->closing && epoch == a_state->epoch) {
			if (deferredRevision != UINT64_MAX)
				a_state->budgetDeferredRevision.store(deferredRevision, std::memory_order_release);
			if (!failure.empty()) {
				a_state->failure = std::move(failure);
				a_state->published.store(nullptr, std::memory_order_release);
				logger::error("DX12 acceleration task failed: {}", a_state->failure);
			} else if (completed) {
				a_state->published.store(std::move(completed), std::memory_order_release);
			}
		}
		a_state->scheduled = false;
		if (!a_state->closing && a_state->failure.empty() && a_state->waitFence &&
			!a_state->waitRegistered && !a_state->waitReady) {
			const auto dependency = FencePoint(a_state->waitFence.get(), a_state->waitValue);
			if (GpuFenceRetired(dependency.fence.get(), dependency.value)) {
				a_state->waitReady = true;
			} else {
				const std::weak_ptr<State> weak = a_state;
				auto* fence = dependency.fence.get();
				const auto value = dependency.value;
				try {
					a_state->waitRegistered = NotifyOnFence(dependency, [weak, fence, value] {
						if (auto current = weak.lock()) {
							{
								std::scoped_lock lock(current->mutex);
								if (current->closing || current->waitFence.get() != fence || current->waitValue != value)
									return;
								current->waitReady = true;
							}
							TrySchedule(current);
						}
					});
				} catch (...) {
					// A failed registration is retried by the active frame path.
					a_state->waitRegistered = false;
				}
			}
		}
	}
}
