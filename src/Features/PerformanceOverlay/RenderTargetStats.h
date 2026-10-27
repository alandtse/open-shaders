#pragma once

#include "Utils/GpuTimestamps.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Draw calls and GPU time per engine render target binding.
 *
 * A binding is the shadow state's depth stencil and first render target at
 * draw time. Its GPU span runs from the first draw into it to the next draw
 * into another binding, idle time included, so work issued between two engine
 * draws counts toward the binding drawn before it. The frame's last span ends
 * at Present.
 */
class RenderTargetStats
{
public:
	/** @brief One binding's smoothed per-frame numbers. */
	struct Row
	{
		std::string name;
		float drawCalls = 0.0f;
		float gpuMs = 0.0f;
	};

	/** @brief Counts one draw into the bound target, opening a new GPU span when the binding changed. Render thread. */
	void CountDraw();

	/** @brief Closes the frame's timing bracket and reads back the frames whose timestamps are ready. Call once per Present. */
	void EndFrame();

	/** @brief Drops every binding, its history and the in-flight timing. */
	void Clear();

	/** @brief Bindings seen so far, highest smoothed GPU time first. */
	std::vector<Row> GetRows() const;

	/** @brief Smoothed GPU time of all timed spans in a frame. */
	float GetTimedMs() const { return smoothTimedMs; }

	/** @brief Share of recent frames whose timestamps resolved, 0..1. */
	float GetResolvedShare() const { return resolvedShare; }

	/** @brief True when the last frame switched binding more often than spans can be timed. */
	bool IsSpanLimitHit() const { return spanLimitHit; }

	/** @brief True when more bindings were seen than the table keeps; their draws are not counted. */
	bool IsBindingLimitHit() const { return bindingLimitHit; }

private:
	static constexpr uint32_t kFrameLatency = 4;
	static constexpr uint32_t kMaxSpans = 256;
	static constexpr size_t kMaxBindings = 64;
	static constexpr float kSmoothing = 0.05f;

	struct Binding
	{
		uint32_t depthStencil = 0;
		uint32_t renderTarget = 0;
		std::string name;
		uint32_t frameDraws = 0;
		float smoothDraws = 0.0f;
		float smoothGpuMs = 0.0f;
	};

	struct FrameSlot
	{
		Util::TimestampQueryBatch batch;
		std::vector<int> spanBinding;
		bool pending = false;
	};

	int FindOrAddBinding(uint32_t a_depthStencil, uint32_t a_renderTarget);
	void BeginSpan(int a_binding);
	void CloseSpan();
	void Resolve(FrameSlot& a_slot, bool a_final);

	std::array<FrameSlot, kFrameLatency> ring;
	std::vector<Binding> bindings;
	std::vector<double> frameMs;
	uint32_t current = 0;
	int currentBinding = -1;
	bool spanOpen = false;
	bool inFrame = false;
	bool configured = false;
	bool spansFull = false;
	bool spanLimitHit = false;
	bool bindingLimitHit = false;
	float smoothTimedMs = 0.0f;
	float resolvedShare = 0.0f;
};
