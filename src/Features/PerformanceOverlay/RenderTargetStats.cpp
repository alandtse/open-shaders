#include "RenderTargetStats.h"

#include "Globals.h"

#include <algorithm>
#include <format>

namespace
{
	std::string TargetName(uint32_t a_index)
	{
		const auto name = magic_enum::enum_name(static_cast<RE::RENDER_TARGETS::RENDER_TARGET>(a_index));
		return name.empty() ? std::format("RT {}", a_index) : std::string(name);
	}

	std::string DepthName(uint32_t a_index)
	{
		const auto name = magic_enum::enum_name(static_cast<RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL>(a_index));
		return name.empty() ? std::format("DS {}", a_index) : std::string(name);
	}

	std::string BindingName(uint32_t a_depthStencil, uint32_t a_renderTarget)
	{
		constexpr uint32_t kNoDepthStencil = static_cast<uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kNONE);
		constexpr uint32_t kNoRenderTarget = static_cast<uint32_t>(RE::RENDER_TARGETS::kNONE);

		std::string name;
		if (a_renderTarget != kNoRenderTarget)
			name = TargetName(a_renderTarget);
		if (a_depthStencil != kNoDepthStencil)
			name += name.empty() ? DepthName(a_depthStencil) + " (depth)" : " + " + DepthName(a_depthStencil) + " (depth)";
		return name.empty() ? "None" : name;
	}
}

int RenderTargetStats::FindOrAddBinding(uint32_t a_depthStencil, uint32_t a_renderTarget)
{
	for (size_t i = 0; i < bindings.size(); ++i) {
		if (bindings[i].depthStencil == a_depthStencil && bindings[i].renderTarget == a_renderTarget)
			return static_cast<int>(i);
	}
	if (bindings.size() >= kMaxBindings)
		return -1;
	bindings.push_back({ a_depthStencil, a_renderTarget, BindingName(a_depthStencil, a_renderTarget) });
	return static_cast<int>(bindings.size()) - 1;
}

void RenderTargetStats::BeginSpan(int a_binding)
{
	auto* context = globals::d3d::context;
	auto* device = globals::d3d::device;
	auto& slot = ring[current];
	if (!inFrame) {
		if (!configured) {
			for (auto& s : ring)
				s.batch.Configure(kMaxSpans, "PerformanceOverlay::RenderTargetTimestamp");
			configured = true;
		}
		slot.batch.Reset();
		slot.spanBinding.clear();
		inFrame = slot.batch.BeginBatch(device, context);
		if (!inFrame)
			return;
	}
	if (spansFull)
		return;

	const int span = slot.batch.BeginInterval(device, context);
	if (span < 0) {
		spansFull = true;
		return;
	}
	if (slot.spanBinding.size() <= static_cast<size_t>(span))
		slot.spanBinding.resize(static_cast<size_t>(span) + 1, -1);
	slot.spanBinding[span] = a_binding;
	spanOpen = true;
}

void RenderTargetStats::CloseSpan()
{
	if (spanOpen)
		ring[current].batch.CommitInterval(globals::d3d::context);
	spanOpen = false;
}

void RenderTargetStats::Resolve(FrameSlot& a_slot, bool a_final)
{
	if (!a_slot.pending)
		return;

	frameMs.assign(bindings.size(), 0.0);
	const auto status = a_slot.batch.TryResolve(globals::d3d::context, [&](uint32_t a_interval, uint64_t a_ticks, uint64_t a_frequency) {
		if (a_interval >= a_slot.spanBinding.size())
			return;
		const int binding = a_slot.spanBinding[a_interval];
		if (binding >= 0 && static_cast<size_t>(binding) < frameMs.size())
			frameMs[binding] += static_cast<double>(a_ticks) * 1000.0 / static_cast<double>(a_frequency);
	});
	if (status == Util::TimestampQueryBatch::Status::NotReady && !a_final)
		return;
	a_slot.pending = false;

	const bool resolved = status == Util::TimestampQueryBatch::Status::Ok;
	resolvedShare = resolvedShare * (1.0f - kSmoothing) + (resolved ? kSmoothing : 0.0f);
	if (!resolved)
		return;

	double timedMs = 0.0;
	for (size_t i = 0; i < bindings.size(); ++i) {
		bindings[i].smoothGpuMs = bindings[i].smoothGpuMs * (1.0f - kSmoothing) + static_cast<float>(frameMs[i]) * kSmoothing;
		timedMs += frameMs[i];
	}
	smoothTimedMs = smoothTimedMs * (1.0f - kSmoothing) + static_cast<float>(timedMs) * kSmoothing;
}

void RenderTargetStats::CountDraw()
{
	auto* shadowState = globals::game::shadowState;
	if (!shadowState || !globals::d3d::context || !globals::d3d::device)
		return;

	const uint32_t depthStencil = globals::game::isVR ? shadowState->GetVRRuntimeData().depthStencil : shadowState->GetRuntimeData().depthStencil;
	const uint32_t renderTarget = static_cast<uint32_t>(globals::game::isVR ? shadowState->GetVRRuntimeData().renderTargets[0] : shadowState->GetRuntimeData().renderTargets[0]);
	if (currentBinding >= 0 && bindings[currentBinding].depthStencil == depthStencil && bindings[currentBinding].renderTarget == renderTarget) {
		bindings[currentBinding].frameDraws++;
		return;
	}

	CloseSpan();
	currentBinding = FindOrAddBinding(depthStencil, renderTarget);
	if (currentBinding < 0) {
		bindingLimitHit = true;
		return;
	}
	bindings[currentBinding].frameDraws++;
	BeginSpan(currentBinding);
}

void RenderTargetStats::EndFrame()
{
	auto* context = globals::d3d::context;
	if (!context)
		return;

	if (inFrame) {
		CloseSpan();
		ring[current].batch.EndBatch(context);
		ring[current].pending = true;
		inFrame = false;
	} else {
		for (auto& binding : bindings)
			binding.smoothGpuMs *= 1.0f - kSmoothing;
		smoothTimedMs *= 1.0f - kSmoothing;
	}
	for (auto& binding : bindings) {
		binding.smoothDraws = binding.smoothDraws * (1.0f - kSmoothing) + static_cast<float>(binding.frameDraws) * kSmoothing;
		binding.frameDraws = 0;
	}
	spanLimitHit = spansFull;
	spansFull = false;
	currentBinding = -1;

	current = (current + 1) % kFrameLatency;
	for (uint32_t i = 0; i < kFrameLatency; ++i)
		Resolve(ring[(current + i) % kFrameLatency], i == 0);
}

void RenderTargetStats::Clear()
{
	if (!configured && bindings.empty())
		return;
	for (auto& slot : ring) {
		slot.batch.ReleaseQueries();
		slot.batch.Reset();
		slot.spanBinding.clear();
		slot.pending = false;
	}
	bindings.clear();
	currentBinding = -1;
	spanOpen = false;
	inFrame = false;
	configured = false;
	spansFull = false;
	spanLimitHit = false;
	bindingLimitHit = false;
	smoothTimedMs = 0.0f;
	resolvedShare = 0.0f;
}

std::vector<RenderTargetStats::Row> RenderTargetStats::GetRows() const
{
	std::vector<Row> rows;
	rows.reserve(bindings.size());
	for (const auto& binding : bindings)
		rows.push_back({ binding.name, binding.smoothDraws, binding.smoothGpuMs });
	std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.gpuMs > b.gpuMs; });
	return rows;
}
