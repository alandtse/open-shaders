#pragma once

#include "FramePlan.h"
#include "RuntimeLayer.h"
#include "Tuning.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace NR
{
	/** @brief The exposure arm a frame resolved to, with what the Prepare pass needs to evaluate it. */
	struct ResolvedExposure
	{
		uint32_t mode = 0;  ///< 0 scalar, 1 the scene exposure, 2 the local estimate.
		float scalar = 1.0f;
		float compensation = 1.0f, rangeMin = 1.0f, rangeMax = 1.0f;
		ID3D11ShaderResourceView* input = nullptr;  ///< Bound at t3; null for the scalar arm.
	};

	/** @brief Everything one frame's passes need, filled by the frame path before it draws. */
	struct FrameContext
	{
		ID3D11ShaderResourceView* guides[4]{};
		ID3D11ComputeShader* encode = nullptr;
		ID3D11ComputeShader* prepare = nullptr;
		ID3D11ComputeShader* stabilize = nullptr;
		ID3D11ComputeShader* composite = nullptr;
		ID3D11ComputeShader* reduce = nullptr;
		ID3D11ComputeShader* adapt = nullptr;
		ExposureGrid grid;
		ResolvedExposure exposure;
		Tuning tuning;
		uint32_t reset = 0;
		uint32_t sourceWidth = 0, sourceHeight = 0;
	};

	/**
	 * @brief The DLSS Neural Rendering pass: one bounded display-proxy Feature 18 evaluation per eye
	 *        at eye render resolution, composited back into kMAIN as a scalar gain before any
	 *        upscaler runs.
	 *
	 * The runtime layer below owns the NGX lifecycle and performs every request on the render
	 * thread; this class owns the frame path, its textures and its history, and never initializes
	 * anything while the feature is off.
	 */
	class NeuralRendering
	{
	public:
		NeuralRendering();
		~NeuralRendering();
		NeuralRendering(const NeuralRendering&) = delete;
		NeuralRendering& operator=(const NeuralRendering&) = delete;

		/**
		 * @brief Runs the pass for one frame, between the pre-upscale post-processing and the upscaler.
		 * @param a_enabled The feature toggle; false only tears down what an earlier frame built.
		 * @param a_tuning Live tuning, sanitized for this frame; commits go through SetTuning.
		 * @param a_renderSize The frame's dynamic render size, before the per-eye split.
		 */
		void DrawBeforeUpscaling(bool a_enabled, const Tuning& a_tuning, float2 a_renderSize);

		/** @brief Invalidates the pass's cached resources, so the next frame rebuilds them. */
		void SetupResources();
		/** @brief Per-frame reset hook: history resets whenever the pass is off or the world is not rendered. */
		void Reset(bool a_enabled);
		/** @brief Invalidates both eyes' temporal histories. */
		void ResetHistory();
		/** @brief Releases the runtime and the pass resources when the owning feature is disabled. */
		void OnRuntimeDisabled();
		/** @brief Drops the cached shaders so the next frame recompiles them. */
		void ClearShaderCache();

		/** @brief Applies a committed tuning, rebuilding the pass only when the runtime-visible part changed. */
		void SetTuning(const Tuning& a_tuning);
		/** @brief The tuning the pass last committed. */
		[[nodiscard]] const Tuning& GetTuning() const { return tuning; }

		/** @brief Queues a retry of a latched failure, applied on the render thread. */
		void RequestRetry() { retryRequested.store(true); }
		/** @brief Queues a history reset, applied on the render thread. */
		void RequestHistoryReset() { resetHistory.store(true); }

		/** @brief Whether the renderer's adapter is an NVIDIA one, so Feature 18 can have a device. */
		[[nodiscard]] bool AdapterSupported();

		/** @brief The plain-language state the settings menu shows, from the live toggle and last frame. */
		[[nodiscard]] DisplayState CurrentDisplayState();

		/**
		 * @brief Whether a retry would be permitted from the current latch and retry budget, as of the
		 *        last frame. False means a restart is the only way to try again.
		 */
		[[nodiscard]] bool CanRetry() const { return retryPermitted.load(); }

		/** @brief Frame outcome, the menu's status line, geometry, NGX results and the nested runtime-layer status. */
		[[nodiscard]] nlohmann::json Status();

		/** @brief Queues a runtime-layer request; the existing nrRuntime* actions drive the layer directly. */
		void Post(RequestKind a_kind) { runtimeLayer.Post(a_kind); }
		/** @brief The runtime layer's own lifecycle status. */
		[[nodiscard]] nlohmann::json RuntimeStatus() { return runtimeLayer.Status(); }
		/**
		 * @brief Performs the layer's queued request, if any.
		 *        Call once per frame on the render thread, before DrawBeforeUpscaling: the queue is
		 *        what initializes the runtime the pass needs, and a drain it runs is CPU-only while
		 *        the pass still holds its own resources.
		 */
		void ProcessPendingRequest() { runtimeLayer.ProcessPendingRequest(); }

	private:
		struct Impl;

		/** @brief Runs one frame's GPU sequence; the caller has already resolved the runtime. */
		void RunFrame(Impl& a_work, const Tuning& a_tuning, float2 a_renderSize);
		/** @brief Picks the exposure arm for this frame and fills the context with it. */
		void ResolveExposure(FrameContext& a_frame);
		/** @brief Allocates the per-frame textures, reallocating when the geometry or format changed. */
		void EnsureResources(Impl& a_work, ID3D11Texture2D* a_color, const D3D11_TEXTURE2D_DESC& a_desc,
			uint32_t a_eyeWidth, uint32_t a_eyeHeight, uint32_t a_eyeCount);

		/** @brief Queues the teardown of the runtime and the pass resources. */
		void BeginTeardown(bool a_reinitialize);
		/** @brief Completes a queued teardown once the layer reports idle, then optionally re-arms it. */
		void FinishTeardown();

		void SetOutcome(Outcome a_outcome);
		void SetFailure(std::string a_text);

		std::unique_ptr<Impl> impl;
		RuntimeLayer runtimeLayer;
		Tuning tuning;

		std::atomic<bool> resetHistory{ true };
		std::atomic<bool> recreate{ false };
		std::atomic<bool> clearShaders{ false };
		std::atomic<bool> retryRequested{ false };
		std::atomic<bool> latched{ false };
		std::atomic<bool> teardownRequested{ false };
		/** True once a teardown has finished, so a disabled frame cannot queue one per frame. */
		bool tornDown = false;
		bool reinitializeAfterTeardown = false;
		std::atomic<bool> adapterProbed{ false };
		std::atomic<bool> adapterNvidia{ false };

		std::atomic<Outcome> outcome{ Outcome::kDisabled };
		std::atomic<uint64_t> appliedFrames{ 0 };
		std::atomic<uint32_t> lastAppliedFrame{ UINT32_MAX };
		std::atomic<float> exposure{ 1.0f };
		/** The arm the last applied frame actually used, after the scene-exposure fallback. */
		std::atomic<uint32_t> resolvedExposureSource{ static_cast<uint32_t>(ExposureSource::kScene) };
		std::atomic<uint32_t> appliedWidth{ 0 };
		std::atomic<uint32_t> appliedHeight{ 0 };
		std::atomic<uint32_t> appliedEyes{ 0 };
		/** The layer's lifecycle latch and the menu's retry affordance, mirrored from the render thread. */
		std::atomic<uint32_t> layerState{ static_cast<uint32_t>(RuntimeState::kNotLoaded) };
		std::atomic<uint32_t> layerFailure{ static_cast<uint32_t>(FailureKind::kNone) };
		std::atomic<bool> retryPermitted{ false };
		/** Mirror of each eye's last NGX result, so a status query never reads the pass's own state. */
		std::atomic<int32_t> ngxResult[2]{ { 0 }, { 0 } };
		/** Mutex-guarded because a latch is written on the render thread and read by a devbench query. */
		std::mutex failureMutex;
		std::string failureText;
	};
}
