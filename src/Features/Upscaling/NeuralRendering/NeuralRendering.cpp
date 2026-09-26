#include "NeuralRendering.h"

#include "ColorContract.h"
#include "D3D12Interop.h"
#include "Parameters.h"
#include "Runtime.h"

#include "Deferred.h"
#include "Feature.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/LazyShader.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace NR
{
	/**
	 * @brief The pass's own resources and per-frame work.
	 *        Everything here is created lazily behind the feature toggle and released when it turns
	 *        off; the NGX runtime and its D3D12 device belong to the runtime layer instead.
	 */
	struct NeuralRendering::Impl
	{
		/** @brief Render-resolution resources for one eye; the shared ones also carry a D3D12 handle. */
		struct Eye
		{
			SharedTexture color, depth, motion, output;
			FrameParameters frame;
			std::unique_ptr<Texture2D> resolved;
			std::array<std::unique_ptr<Texture2D>, 2> residualHistory;
			std::unique_ptr<Texture2D> depthHistory;
			uint32_t residualIndex = 0;
			bool residualValid = false;
		};

		/** @brief Matches the NRColor cbuffer in ColorTransferCS.hlsl; one row per 16 bytes. */
		struct alignas(16) ColorTransferData
		{
			uint32_t width = 0, height = 0, eyeOffsetX = 0, historyValid = 0;
			uint32_t exposureMode = 0;
			float exposureScalar = 1.0f, exposureCompensation = 1.0f, exposureRangeMin = 1.0f;
			float exposureRangeMax = 1.0f, toneMaxStops = Color::kMaxToneStops, temporalAlpha = Color::kTemporalAlpha, temporalSigmaClip = Color::kTemporalSigmaClip;
			float temporalClamp = Color::kTemporalClamp, depthRejectThreshold = Color::kDepthRejectThreshold, pad0 = 0.0f, pad1 = 0.0f;
		};
		static_assert(offsetof(ColorTransferData, exposureMode) == 16);
		static_assert(offsetof(ColorTransferData, exposureRangeMax) == 32);
		static_assert(offsetof(ColorTransferData, temporalClamp) == 48);
		static_assert(sizeof(ColorTransferData) == 64);

		/** @brief Matches the NRExposure cbuffer in NRExposureCS.hlsl. */
		struct alignas(16) ExposureData
		{
			uint32_t sourceWidth = 0, sourceHeight = 0, groupColumns = 0, groupRows = 0;
			uint32_t resetAdaptation = 0;
			float minLuminance = Color::kExposureMinLum, maxLuminance = Color::kExposureMaxLum, pad0 = 0.0f;
			float deltaTime = 0.0f, tau = Color::kExposureTau, pad1 = 0.0f, pad2 = 0.0f;
		};
		static_assert(offsetof(ExposureData, resetAdaptation) == 16);
		static_assert(offsetof(ExposureData, minLuminance) == 20);
		static_assert(offsetof(ExposureData, deltaTime) == 32);
		static_assert(sizeof(ExposureData) == 48);

		/** @brief Private D3D11 state, so NR's own pipeline cannot leak into the engine's. */
		struct ContextScope
		{
			ContextScope(ID3D11DeviceContext1* a_context, ID3DDeviceContextState* a_isolated) :
				context(a_context)
			{
				context->SwapDeviceContextState(a_isolated, previous.put());
				context->ClearState();
			}
			~ContextScope()
			{
				context->ClearState();
				context->SwapDeviceContextState(previous.get(), nullptr);
			}
			ContextScope(const ContextScope&) = delete;
			ContextScope& operator=(const ContextScope&) = delete;

			ID3D11DeviceContext1* context;
			winrt::com_ptr<ID3DDeviceContextState> previous;
		};

		/** @brief Structured-buffer descriptor for the exposure reduction's own buffers. */
		static D3D11_BUFFER_DESC ExposureBufferDesc(uint32_t a_count)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(sizeof(float) * a_count);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(float);
			return desc;
		}

		/** @brief SRV over a_count floats of a float structured buffer. */
		static D3D11_SHADER_RESOURCE_VIEW_DESC StructuredSRVDesc(uint32_t a_count)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = 0;
			desc.Buffer.NumElements = a_count;
			return desc;
		}

		/** @brief UAV over a_count floats of a float structured buffer. */
		static D3D11_UNORDERED_ACCESS_VIEW_DESC StructuredUAVDesc(uint32_t a_count)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = 0;
			desc.Buffer.NumElements = a_count;
			return desc;
		}

		std::array<Eye, 2> eyes;
		std::unique_ptr<Texture2D> original;
		std::array<std::unique_ptr<Texture2D>, 2> encodeMasks;
		winrt::com_ptr<ID3D11DeviceContext1> context;
		winrt::com_ptr<ID3DDeviceContextState> isolated;
		winrt::com_ptr<ID3D11SamplerState> temporalSampler;
		std::unique_ptr<ConstantBuffer> colorBuffer;
		std::unique_ptr<ConstantBuffer> encodeBuffer;
		std::unique_ptr<ConstantBuffer> exposureBuffer;
		std::unique_ptr<Buffer> exposurePartials;
		std::unique_ptr<Buffer> exposureState;
		Util::LazyShader<ID3D11ComputeShader> prepareColor, stabilizeResidual, compositeColor, exposureReduce, exposureAdapt;

		winrt::com_ptr<ID3D11Texture2D> source;
		uint32_t width = 0, height = 0, guideWidth = 0, guideHeight = 0, eyeCount = 0, lastFrame = UINT32_MAX;
		/** Interop device generation the shared textures were opened on. */
		uint32_t deviceGeneration = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		bool ready = false;
		/** Cleared with the history, so the next local estimate snaps instead of adapting. */
		bool exposureAdapted = false;
		FrameContext frame;
		/** The pass, so the frame path can reach the runtime layer it does not own. */
		NeuralRendering* owner = nullptr;

		/** @brief Creates the device-independent objects the frame path needs; the layer owns the rest. */
		void Initialize()
		{
			winrt::com_ptr<ID3D11Device1> device;
			winrt::check_hresult(globals::d3d::device->QueryInterface(device.put()));
			winrt::check_hresult(globals::d3d::context->QueryInterface(context.put()));
			const auto level = globals::d3d::device->GetFeatureLevel();
			winrt::check_hresult(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
				__uuidof(ID3D11Device), nullptr, isolated.put()));
			Util::SetResourceName(isolated.get(), "NeuralRendering::ContextState");
			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			winrt::check_hresult(globals::d3d::device->CreateSamplerState(&samplerDesc, temporalSampler.put()));
			Util::SetResourceName(temporalSampler.get(), "NeuralRendering::TemporalResidual Sampler");
			colorBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorTransferData>(), "NeuralRendering::ColorTransfer CB");
			encodeBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Upscaling::UpscalingDataCB>(), "NeuralRendering::Encode CB");
			exposureBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ExposureData>(), "NeuralRendering::Exposure CB");
			ready = true;
		}

		/** @brief Releases every per-frame resource; the caller must have drained the GPU first. */
		void Release()
		{
			eyes = {};
			original.reset();
			encodeMasks = {};
			exposurePartials.reset();
			exposureState.reset();
			source = nullptr;
			width = height = guideWidth = guideHeight = eyeCount = 0;
			lastFrame = UINT32_MAX;
			format = DXGI_FORMAT_UNKNOWN;
			exposureAdapted = false;
		}

		/** @brief Drops the cached shaders so the next frame recompiles them. */
		void ResetShaders()
		{
			prepareColor.Reset();
			stabilizeResidual.Reset();
			compositeColor.Reset();
			exposureReduce.Reset();
			exposureAdapt.Reset();
		}

		/**
		 * @brief Binds one constant buffer plus an SRV and a UAV range, all from index 0.
		 *        The UAVs go first: D3D11 nulls an SRV whose resource is still bound as a UAV, and
		 *        each pass here re-reads a texture the previous one wrote.
		 * @param a_withFeatureData False only for a shader that reads no Linear Lighting or ACEScg
		 *        flag, which lives in the b6 buffer.
		 */
		void BindPass(ID3D11Buffer* a_constantBuffer, std::initializer_list<ID3D11ShaderResourceView*> a_inputs,
			std::initializer_list<ID3D11UnorderedAccessView*> a_outputs, bool a_withFeatureData)
		{
			std::array<ID3D11ShaderResourceView*, 8> inputs{};
			std::array<ID3D11UnorderedAccessView*, 8> outputs{};
			std::copy(a_inputs.begin(), a_inputs.end(), inputs.begin());
			std::copy(a_outputs.begin(), a_outputs.end(), outputs.begin());
			context->CSSetConstantBuffers(0, 1, &a_constantBuffer);
			globals::state->BindSharedDataCS(context.get(), a_withFeatureData);
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
			context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
		}

		/** @brief Runs every NR pass for one frame; the caller has already resolved the runtime. */
		void Draw(ID3D11Texture2D* a_color, const FrameContext& a_frame)
		{
			frame = a_frame;
			ContextScope scope(context.get(), isolated.get());
			const D3D11_BOX sourceBox{ 0, 0, 0, frame.sourceWidth, frame.sourceHeight, 1 };
			context->CopySubresourceRegion(original->resource.get(), 0, 0, 0, 0, a_color, 0, &sourceBox);

			DispatchExposure();
			{
				CS_GPU_PASS("Upscaling::NRPrepare");
				for (uint32_t eye = 0; eye < eyeCount; ++eye)
					PrepareEye(eye);
			}
			EncodeGuides();
			EvaluateEyes();
			{
				CS_GPU_PASS("Upscaling::NRStabilize");
				for (uint32_t eye = 0; eye < eyeCount; ++eye)
					StabilizeEye(eye);
			}
			{
				CS_GPU_PASS("Upscaling::NRComposite");
				for (uint32_t eye = 0; eye < eyeCount; ++eye)
					CompositeEye(eye);
			}
			// A bound UAV cannot be a copy source, and the composite leaves one bound.
			context->ClearState();
			const D3D11_BOX eyeBox{ 0, 0, 0, width, height, 1 };
			for (uint32_t eye = 0; eye < eyeCount; ++eye)
				context->CopySubresourceRegion(a_color, 0, eye * width, 0, 0, eyes[eye].resolved->resource.get(), 0, &eyeBox);
		}

		/** @brief Local exposure estimate over both eyes, so left and right share one value. */
		void DispatchExposure()
		{
			if (frame.exposure.mode != 2)
				return;
			CS_GPU_PASS("Upscaling::NRExposure");
			ExposureData data;
			data.sourceWidth = frame.sourceWidth;
			data.sourceHeight = frame.sourceHeight;
			data.groupColumns = frame.grid.columns;
			data.groupRows = frame.grid.rows;
			data.resetAdaptation = exposureAdapted ? 0u : 1u;
			data.deltaTime = std::max(*globals::game::deltaTime, 0.0f);
			exposureBuffer->Update(data);
			BindPass(exposureBuffer->CB(), { original->srv.get() },
				{ exposurePartials->uav.get(), exposureState->uav.get() }, true);
			context->CSSetShader(frame.reduce, nullptr, 0);
			context->Dispatch(frame.grid.columns, frame.grid.rows, 1);
			context->CSSetShader(frame.adapt, nullptr, 0);
			context->Dispatch(1, 1, 1);
			context->ClearState();
			exposureAdapted = true;
		}

		/** @brief Builds one eye's bounded display proxy from the stereo kMAIN copy. */
		void PrepareEye(uint32_t a_eye)
		{
			ColorTransferData data;
			data.width = width;
			data.height = height;
			data.eyeOffsetX = a_eye * width;
			data.exposureMode = frame.exposure.mode;
			data.exposureScalar = frame.exposure.scalar;
			data.exposureCompensation = frame.exposure.compensation;
			data.exposureRangeMin = frame.exposure.rangeMin;
			data.exposureRangeMax = frame.exposure.rangeMax;
			colorBuffer->Update(data);
			BindPass(colorBuffer->CB(), { original->srv.get(), nullptr, nullptr, frame.exposure.input },
				{ eyes[a_eye].color.texture->uav.get() }, true);
			context->CSSetShader(frame.prepare, nullptr, 0);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			context->ClearState();
		}

		/**
		 * @brief Encodes this frame's guides into zero-origin per-eye textures.
		 *        NR compiles the encoder without the DLSS define, so motion is a raw copy and the
		 *        depth and motion guides describe the same pixel.
		 */
		void EncodeGuides()
		{
			CS_GPU_PASS("Upscaling::NREncodeGuides");
			Upscaling::UpscalingDataCB data{ { static_cast<float>(guideWidth), static_cast<float>(guideHeight) }, 0, 0 };
			for (uint32_t eye = 0; eye < eyeCount; ++eye) {
				auto& target = eyes[eye];
				data.eyeOffsetX = eye * guideWidth;
				encodeBuffer->Update(data);
				// The shared encoder writes both masks even though NR consumes only motion and depth.
				BindPass(encodeBuffer->CB(), { frame.guides[0], frame.guides[1], frame.guides[2], frame.guides[3] },
					{ encodeMasks[0]->uav.get(), encodeMasks[1]->uav.get(), target.motion.texture->uav.get(), target.depth.texture->uav.get() }, true);
				context->CSSetShader(frame.encode, nullptr, 0);
				context->Dispatch((guideWidth + 7) / 8, (guideHeight + 7) / 8, 1);
				if (frame.reset) {
					// A reset frame has no history to reproject into, so a stale motion field must go.
					constexpr float zero[4]{};
					context->ClearUnorderedAccessViewFloat(target.motion.texture->uav.get(), zero);
				}
			}
			// The evaluation below writes these same textures through D3D12, so none may stay bound.
			context->ClearState();
		}

		/** @brief Creates or evaluates both eyes' Feature 18 instances on the interop command list. */
		void EvaluateEyes()
		{
			CS_GPU_PASS("Upscaling::NREvaluate");
			// A history reset must not overlap the evaluation it resets.
			if (frame.reset)
				owner->runtimeLayer.Interop().Drain();
			auto& interop = owner->runtimeLayer.Interop();
			auto* commands = interop.Begin();
			bool success = true;
			try {
				for (uint32_t eye = 0; eye < eyeCount && success; ++eye) {
					auto& target = eyes[eye];
					Transition(commands, target, true);
					GuideParameters guides;
					guides.depth = GuideRegion{ 0, 0, guideWidth, guideHeight };
					guides.motion = GuideRegion{ 0, 0, guideWidth, guideHeight };
					// Motion vectors hold normalized eye-UV displacement; Feature 18 takes input pixels.
					guides.motionScaleX = static_cast<float>(width);
					guides.motionScaleY = static_cast<float>(height);
					target.frame.reset = frame.reset != 0;
					success = owner->runtimeLayer.NgxRuntime().Evaluate(commands, eye, target.color.resource.get(),
						target.depth.resource.get(), target.motion.resource.get(), target.output.resource.get(),
						width, height, guides, target.frame, frame.tuning);
					Transition(commands, target, false);
				}
			} catch (...) {
				// A list left open would block the next Begin(), which resets its allocator.
				interop.End();
				throw;
			}
			interop.End();
			if (!success)
				throw std::runtime_error(std::format("Feature 18 evaluation failed (NGX L/R: 0x{:08X}/0x{:08X})",
					eyes[0].frame.result, eyeCount > 1 ? eyes[1].frame.result : 0u));
		}

		/** @brief Transitions one eye's shared textures into, or out of, the states its passes use. */
		static void Transition(ID3D12GraphicsCommandList* a_commands, Eye& a_eye, bool a_enter)
		{
			ID3D12Resource* resources[]{ a_eye.color.resource.get(), a_eye.depth.resource.get(), a_eye.motion.resource.get(), a_eye.output.resource.get() };
			D3D12_RESOURCE_BARRIER barriers[4]{};
			for (uint32_t index = 0; index < 4; ++index) {
				const auto state = index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[index].Transition = { resources[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
					a_enter ? D3D12_RESOURCE_STATE_COMMON : state, a_enter ? state : D3D12_RESOURCE_STATE_COMMON };
			}
			a_commands->ResourceBarrier(4, barriers);
		}

		/** @brief Stabilizes one eye's log2 tone residual against its reprojected history. */
		void StabilizeEye(uint32_t a_eye)
		{
			auto& target = eyes[a_eye];
			const auto readIndex = target.residualIndex;
			const auto writeIndex = readIndex ^ 1u;
			ColorTransferData data;
			data.width = width;
			data.height = height;
			data.eyeOffsetX = a_eye * width;
			data.historyValid = target.residualValid && !target.frame.reset;
			colorBuffer->Update(data);
			BindPass(colorBuffer->CB(),
				{ nullptr, target.color.texture->srv.get(), target.output.texture->srv.get(), nullptr,
					target.residualHistory[readIndex]->srv.get(), target.motion.texture->srv.get(), target.depth.texture->srv.get(), target.depthHistory->srv.get() },
				{ nullptr, nullptr, target.residualHistory[writeIndex]->uav.get() }, false);
			ID3D11SamplerState* sampler = temporalSampler.get();
			context->CSSetSamplers(0, 1, &sampler);
			context->CSSetShader(frame.stabilize, nullptr, 0);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			context->ClearState();
			context->CopyResource(target.depthHistory->resource.get(), target.depth.texture->resource.get());
			target.residualIndex = writeIndex;
			target.residualValid = true;
		}

		/** @brief Applies one eye's stabilized residual to the original as a scalar gain. */
		void CompositeEye(uint32_t a_eye)
		{
			auto& target = eyes[a_eye];
			ColorTransferData data;
			data.width = width;
			data.height = height;
			data.eyeOffsetX = a_eye * width;
			colorBuffer->Update(data);
			BindPass(colorBuffer->CB(),
				{ original->srv.get(), nullptr, target.output.texture->srv.get(), nullptr, target.residualHistory[target.residualIndex]->srv.get() },
				{ target.resolved->uav.get() }, true);
			context->CSSetShader(frame.composite, nullptr, 0);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		}
	};
}

namespace
{
	using namespace NR;

	/** @brief kMAIN's format must be one Feature 18 accepts as its color input. */
	bool IsSupportedSourceFormat(DXGI_FORMAT a_format)
	{
		return a_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
		       a_format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		       a_format == DXGI_FORMAT_R10G10B10A2_UNORM ||
		       a_format == DXGI_FORMAT_R11G11B10_FLOAT;
	}
}

NR::NeuralRendering::NeuralRendering() :
	impl(std::make_unique<Impl>())
{
	impl->owner = this;
}

NR::NeuralRendering::~NeuralRendering() = default;

bool NR::NeuralRendering::AdapterSupported()
{
	if (!adapterProbed.load()) {
		bool nvidia = false;
		winrt::com_ptr<IDXGIDevice> dxgi;
		winrt::com_ptr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc{};
		if (globals::d3d::device &&
			SUCCEEDED(globals::d3d::device->QueryInterface(dxgi.put())) &&
			SUCCEEDED(dxgi->GetAdapter(adapter.put())) &&
			SUCCEEDED(adapter->GetDesc(&desc))) {
			nvidia = desc.VendorId == Streamline::kNvidiaVendorId;
		}
		adapterNvidia.store(nvidia);
		adapterProbed.store(true);
	}
	return adapterNvidia.load();
}

void NR::NeuralRendering::SetupResources()
{
	adapterProbed.store(false);
	recreate.store(true);
	resetHistory.store(true);
}

void NR::NeuralRendering::ResetHistory()
{
	resetHistory.store(true);
}

void NR::NeuralRendering::ClearShaderCache()
{
	clearShaders.store(true);
	// A shader that failed to compile latched the pass, so clearing the cache must let it retry.
	retryRequested.store(true);
	resetHistory.store(true);
}

void NR::NeuralRendering::OnRuntimeDisabled()
{
	// The teardown itself must run where the D3D11 context lives, so only the request lands here.
	BeginTeardown(false);
}

void NR::NeuralRendering::Reset(bool a_enabled)
{
	if (!a_enabled || !globals::state->worldRenderedThisFrame)
		resetHistory.store(true);
}

void NR::NeuralRendering::SetTuning(const Tuning& a_tuning)
{
	auto sanitized = a_tuning;
	sanitized.Sanitize();
	// Every key WriteCreation sets is latched when Feature 18 is created, so any commit other than
	// the per-frame exposure arm must retire the handle; the exposure arm needs only a new history.
	if (sanitized.exposureSource != tuning.exposureSource)
		resetHistory.store(true);
	if (sanitized.style != tuning.style || sanitized.intensity != tuning.intensity ||
		sanitized.localToneStrength != tuning.localToneStrength ||
		sanitized.localStructureStrength != tuning.localStructureStrength ||
		sanitized.skinStructureStrength != tuning.skinStructureStrength ||
		sanitized.useAutoMask != tuning.useAutoMask || sanitized.proxyContract != tuning.proxyContract) {
		recreate.store(true);
		resetHistory.store(true);
	}
	tuning = sanitized;
}

void NR::NeuralRendering::SetOutcome(Outcome a_outcome)
{
	outcome.store(a_outcome);
	// Mirrored here because this runs on the render thread: a status query on any other thread
	// reads these instead of the layer's own state.
	layerState.store(static_cast<uint32_t>(runtimeLayer.LayerState()));
	layerFailure.store(static_cast<uint32_t>(runtimeLayer.LayerFailure()));
	// A pass-level latch is rebuilt without spending the layer's retry budget, so it is always retryable.
	retryPermitted.store(latched.load() || runtimeLayer.RetryPermitted());
}

NR::DisplayState NR::NeuralRendering::CurrentDisplayState()
{
	// The toggle is read live, so the menu never reports Off for a setting it just turned on and
	// has not reached a frame yet.
	return ClassifyDisplayState(globals::features::upscaling.settings.neuralRenderingEnabled, AdapterSupported(),
		static_cast<RuntimeState>(layerState.load()), static_cast<FailureKind>(layerFailure.load()), latched.load());
}

void NR::NeuralRendering::SetFailure(std::string a_text)
{
	std::scoped_lock lock(failureMutex);
	failureText = std::move(a_text);
}

void NR::NeuralRendering::BeginTeardown(bool a_reinitialize)
{
	teardownRequested.store(true);
	reinitializeAfterTeardown = a_reinitialize;
}

void NR::NeuralRendering::FinishTeardown()
{
	if (!teardownRequested.load())
		return;
	// A shutdown drains before it releases, so only a live layer is worth waiting for. Re-posting is
	// free (the queue replaces an untaken request) and covers a request that replaced the shutdown.
	if (runtimeLayer.LayerState() == RuntimeState::kInitialized) {
		runtimeLayer.Post(RequestKind::kShutdown);
		SetOutcome(Outcome::kInitializing);
		return;
	}
	teardownRequested.store(false);
	tornDown = true;
	impl->Release();
	impl->ready = false;
	latched.store(false);
	{
		std::scoped_lock lock(failureMutex);
		failureText.clear();
	}
	// One line per disable, and only for a pass that had actually composited a frame.
	if (!reinitializeAfterTeardown && appliedFrames.load())
		logger::info("[NeuralRendering] disabled: runtime and pass resources released");
	if (reinitializeAfterTeardown) {
		reinitializeAfterTeardown = false;
		retryRequested.store(true);
	}
	resetHistory.store(true);
}

void NR::NeuralRendering::DrawBeforeUpscaling(bool a_enabled, const Tuning& a_tuning, float2 a_renderSize)
{
	FinishTeardown();
	if (teardownRequested.load())
		return;

	FrameInputs inputs;
	inputs.enabled = a_enabled;
	inputs.worldRendered = globals::state->worldRenderedThisFrame;
	// The adapter probe is a one-time DXGI query, so an off or menu frame pays nothing for it.
	inputs.adapterSupported = a_enabled && inputs.worldRendered ? AdapterSupported() : false;
	inputs.failed = latched.load();
	inputs.retryRequested = retryRequested.load();

	switch (DecideFrameAction(inputs)) {
	case FrameAction::kSkipDisabled:
		SetOutcome(Outcome::kDisabled);
		// Once is enough: a disabled frame has nothing left to release, and the layer only needs
		// one shutdown request to drop the runtime and its device.
		if (!tornDown)
			BeginTeardown(false);
		resetHistory.store(true);
		return;
	case FrameAction::kSkipNoWorld:
		SetOutcome(Outcome::kNoWorld);
		resetHistory.store(true);
		return;
	case FrameAction::kSkipUnsupported:
		SetOutcome(Outcome::kUnsupportedAdapter);
		return;
	case FrameAction::kSkipLatched:
		SetOutcome(Outcome::kFailed);
		return;
	case FrameAction::kRetry:
		retryRequested.store(false);
		if (runtimeLayer.LayerState() == RuntimeState::kFailed)
			runtimeLayer.Post(RequestKind::kRetry);
		else if (latched.load())
			BeginTeardown(true);
		SetOutcome(Outcome::kInitializing);
		return;
	case FrameAction::kRun:
		break;
	}

	if (runtimeLayer.LayerState() == RuntimeState::kFailed) {
		SetOutcome(Outcome::kFailed);
		return;
	}
	if (runtimeLayer.LayerState() != RuntimeState::kInitialized) {
		// Initialization is a layer request like any other, so it lands on the render thread
		// through the same queue; a repeat post collapses into the one already queued.
		runtimeLayer.Post(RequestKind::kInitialize);
		SetOutcome(Outcome::kInitializing);
		return;
	}

	try {
		auto sanitized = a_tuning;
		sanitized.Sanitize();
		RunFrame(*impl, sanitized, a_renderSize);
	} catch (const winrt::hresult_error& e) {
		latched.store(true);
		SetFailure(std::format("NR paused: {}. Retry neural rendering after correcting the error.", winrt::to_string(e.message())));
		logger::error("[NeuralRendering] D3D call failed: 0x{:08X}", static_cast<uint32_t>(e.code().value));
		SetOutcome(Outcome::kFailed);
	} catch (const std::exception& e) {
		latched.store(true);
		SetFailure(std::format("NR paused: {}. Retry neural rendering after correcting the error.", e.what()));
		logger::error("[NeuralRendering] {}", e.what());
		SetOutcome(Outcome::kFailed);
	}
}

void NR::NeuralRendering::RunFrame(Impl& a_work, const Tuning& a_tuning, float2 a_renderSize)
{
	// A second entry in one frame would re-apply the gain to an already-composited kMAIN.
	if (a_work.lastFrame == globals::state->frameCount)
		return;
	CS_GPU_PASS("Upscaling::NeuralRendering");

	auto& upscaling = globals::features::upscaling;
	auto* color = Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture);
	if (!color)
		throw std::runtime_error("kMAIN has no texture to read a neural-rendering source from");

	Upscaling::EncodeInputViews guides{};
	const char* missingGuide = nullptr;
	if (!upscaling.GetEncodeInputs(guides, missingGuide))
		throw std::runtime_error(std::format("a neural-rendering guide texture is missing: {}", missingGuide ? missingGuide : "unknown"));

	const auto eyeCount = globals::game::isVR ? 2u : 1u;
	const auto eyeWidth = EyeRenderWidth(a_renderSize.x, eyeCount);
	const auto eyeHeight = static_cast<uint32_t>(a_renderSize.y);
	D3D11_TEXTURE2D_DESC sourceDesc{};
	color->GetDesc(&sourceDesc);
	if (!eyeWidth || !eyeHeight || sourceDesc.Width < eyeWidth * eyeCount || sourceDesc.Height < eyeHeight ||
		sourceDesc.ArraySize != 1 || sourceDesc.SampleDesc.Count != 1 || !IsSupportedSourceFormat(sourceDesc.Format)) {
		throw std::runtime_error(std::format("unsupported neural-rendering source: {}x{}, DXGI format {}, array {}, {} sample(s)",
			sourceDesc.Width, sourceDesc.Height, static_cast<uint32_t>(sourceDesc.Format), sourceDesc.ArraySize, sourceDesc.SampleDesc.Count));
	}
	for (auto* guide : guides) {
		D3D11_TEXTURE2D_DESC desc{};
		if (!guide || !Util::GetTexture2DDesc(guide, desc) || desc.Width < eyeWidth * eyeCount || desc.Height < eyeHeight ||
			desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
			throw std::runtime_error("a neural-rendering guide texture is missing or too small");
		}
	}

	if (!a_work.ready)
		a_work.Initialize();
	if (clearShaders.exchange(false))
		a_work.ResetShaders();
	EnsureResources(a_work, color, sourceDesc, eyeWidth, eyeHeight, eyeCount);

	FrameContext frame;
	frame.tuning = a_tuning;
	for (uint32_t index = 0; index < 4; ++index)
		frame.guides[index] = guides[index];
	frame.sourceWidth = a_work.width * a_work.eyeCount;
	frame.sourceHeight = a_work.height;
	frame.encode = upscaling.GetEncodeTexturesCS(Upscaling::UpscaleMethod::kNONE, Upscaling::EncodeOutput::kTypedDepth);
	frame.prepare = a_work.prepareColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
		{}, "cs_5_0", "Prepare", "NeuralRendering::PrepareColor CS");
	frame.stabilize = a_work.stabilizeResidual.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
		{}, "cs_5_0", "StabilizeResidual", "NeuralRendering::StabilizeResidual CS");
	frame.composite = a_work.compositeColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
		{}, "cs_5_0", "Composite", "NeuralRendering::CompositeHDR CS");
	if (!frame.encode || !frame.prepare || !frame.stabilize || !frame.composite)
		throw std::runtime_error("a neural-rendering shader failed to compile");
	frame.grid = ComputeExposureGrid(frame.sourceWidth, frame.sourceHeight);
	if (!frame.grid.Empty()) {
		frame.reduce = a_work.exposureReduce.Get(L"Data/Shaders/Upscaling/NeuralRendering/NRExposureCS.hlsl",
			{}, "cs_5_0", "Reduce", "NeuralRendering::ExposureReduce CS");
		frame.adapt = a_work.exposureAdapt.Get(L"Data/Shaders/Upscaling/NeuralRendering/NRExposureCS.hlsl",
			{}, "cs_5_0", "Adapt", "NeuralRendering::ExposureAdapt CS");
		if (!frame.reduce || !frame.adapt)
			throw std::runtime_error("a neural-rendering exposure shader failed to compile");
	}
	ResolveExposure(frame);

	// A first frame, a skipped frame and an explicit reset all start a new history.
	const bool reset = NeedsHistoryReset(resetHistory.exchange(false), a_work.lastFrame, globals::state->frameCount);
	frame.reset = reset ? 1u : 0u;
	if (reset)
		a_work.exposureAdapted = false;

	a_work.Draw(color, frame);

	const auto mode = frame.exposure.mode;
	exposure.store(mode == 0 ? frame.exposure.scalar : -1.0f);
	resolvedExposureSource.store(static_cast<uint32_t>(mode == 1 ? ExposureSource::kScene : mode == 2 ? ExposureSource::kLocal :
																										ExposureSource::kScalar));
	appliedWidth.store(a_work.width);
	appliedHeight.store(a_work.height);
	appliedEyes.store(a_work.eyeCount);
	for (uint32_t eye = 0; eye < 2; ++eye)
		ngxResult[eye].store(static_cast<int32_t>(a_work.eyes[eye].frame.result));
	lastAppliedFrame.store(globals::state->frameCount);
	appliedFrames.fetch_add(1);
	a_work.lastFrame = globals::state->frameCount;
	// The pass holds live resources again, so a later disable must tear them down.
	tornDown = false;
	SetOutcome(Outcome::kApplied);
}

void NR::NeuralRendering::ResolveExposure(FrameContext& a_frame)
{
	Feature::SceneExposure scene;
	const bool sceneAvailable = Feature::FindSceneExposure(scene) && scene.adaptedLuminance != nullptr;
	switch (ResolveExposureSource(static_cast<ExposureSource>(a_frame.tuning.exposureSource), sceneAvailable)) {
	case ExposureSource::kScene:
		a_frame.exposure.mode = 1;
		a_frame.exposure.input = scene.adaptedLuminance;
		a_frame.exposure.compensation = scene.compensationScale;
		a_frame.exposure.rangeMin = scene.luminanceRange.x;
		a_frame.exposure.rangeMax = scene.luminanceRange.y;
		return;
	case ExposureSource::kLocal:
		if (!a_frame.grid.Empty()) {
			a_frame.exposure.mode = 2;
			a_frame.exposure.input = impl->exposureState->srv.get();
			return;
		}
		break;
	case ExposureSource::kScalar:
	case ExposureSource::kCount:
		break;
	}
	a_frame.exposure.mode = 0;
	a_frame.exposure.scalar = 1.0f;
	a_frame.exposure.input = nullptr;
}

void NR::NeuralRendering::EnsureResources(Impl& a_work, ID3D11Texture2D* a_color, const D3D11_TEXTURE2D_DESC& a_desc, uint32_t a_eyeWidth, uint32_t a_eyeHeight, uint32_t a_eyeCount)
{
	auto& interop = runtimeLayer.Interop();
	const bool forceRecreate = recreate.exchange(false);
	const auto deviceGeneration = interop.Generation();
	const bool deviceChanged = a_work.deviceGeneration != deviceGeneration;
	const bool geometryChanged = a_work.width != a_eyeWidth || a_work.height != a_eyeHeight ||
	                             a_work.eyeCount != a_eyeCount || a_work.format != a_desc.Format;
	if (!forceRecreate && !geometryChanged && !deviceChanged) {
		a_work.source.copy_from(a_color);
		return;
	}
	if (a_work.width) {
		// The previous frame's commands may still read the old textures, so retire them first.
		interop.Drain();
	}
	// Feature 18 latches its creation size, flags and selectors, so its handles go with the textures.
	runtimeLayer.NgxRuntime().ResetFeatures();
	a_work.Release();

	D3D11_TEXTURE2D_DESC colorDesc{};
	colorDesc.Width = a_eyeWidth * a_eyeCount;
	colorDesc.Height = a_eyeHeight;
	colorDesc.Format = a_desc.Format;
	colorDesc.MipLevels = colorDesc.ArraySize = colorDesc.SampleDesc.Count = 1;
	colorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = colorDesc.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	a_work.original = std::make_unique<Texture2D>(colorDesc, "NeuralRendering::OriginalHDR");
	a_work.original->CreateSRV(srv);

	D3D11_TEXTURE2D_DESC maskDesc = colorDesc;
	maskDesc.Width = a_eyeWidth;
	maskDesc.Height = a_eyeHeight;
	maskDesc.Format = DXGI_FORMAT_R8_UNORM;
	maskDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	D3D11_UNORDERED_ACCESS_VIEW_DESC maskUAV{};
	maskUAV.Format = maskDesc.Format;
	maskUAV.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	for (uint32_t index = 0; index < a_work.encodeMasks.size(); ++index) {
		a_work.encodeMasks[index] = std::make_unique<Texture2D>(maskDesc, std::format("NeuralRendering::EncodeMask{}", index).c_str());
		a_work.encodeMasks[index]->CreateUAV(maskUAV);
	}

	// Per-eye color is that eye's whole render region; its guides are the same extent.
	D3D11_TEXTURE2D_DESC perEye = maskDesc;
	perEye.Format = colorDesc.Format;
	D3D11_UNORDERED_ACCESS_VIEW_DESC colorUAV = maskUAV;
	colorUAV.Format = colorDesc.Format;
	for (uint32_t eye = 0; eye < a_eyeCount; ++eye) {
		auto& target = a_work.eyes[eye];
		const auto name = std::format("NeuralRendering::Eye{}", eye);
		target.resolved = std::make_unique<Texture2D>(perEye, (name + " ResolvedHDR").c_str());
		target.resolved->CreateUAV(colorUAV);
		srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		target.color = interop.CreateTexture(a_eyeWidth, a_eyeHeight, srv.Format, name + " HDRInput");
		target.color.texture->CreateSRV(srv);
		target.depth = interop.CreateTexture(a_eyeWidth, a_eyeHeight, DXGI_FORMAT_R32_FLOAT, name + " Depth");
		srv.Format = DXGI_FORMAT_R32_FLOAT;
		target.depth.texture->CreateSRV(srv);
		target.motion = interop.CreateTexture(a_eyeWidth, a_eyeHeight, DXGI_FORMAT_R16G16_FLOAT, name + " Motion");
		srv.Format = DXGI_FORMAT_R16G16_FLOAT;
		target.motion.texture->CreateSRV(srv);
		srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		target.output = interop.CreateTexture(a_eyeWidth, a_eyeHeight, srv.Format, name + " HDROutput");
		target.output.texture->CreateSRV(srv);

		D3D11_TEXTURE2D_DESC historyDesc = perEye;
		historyDesc.Format = DXGI_FORMAT_R16_FLOAT;
		historyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SHADER_RESOURCE_VIEW_DESC historySRV = srv;
		historySRV.Format = historyDesc.Format;
		D3D11_UNORDERED_ACCESS_VIEW_DESC historyUAV = colorUAV;
		historyUAV.Format = historyDesc.Format;
		for (uint32_t index = 0; index < target.residualHistory.size(); ++index) {
			target.residualHistory[index] = std::make_unique<Texture2D>(historyDesc, std::format("{} ResidualHistory{}", name, index).c_str());
			target.residualHistory[index]->CreateSRV(historySRV);
			target.residualHistory[index]->CreateUAV(historyUAV);
		}
		D3D11_TEXTURE2D_DESC depthHistoryDesc = historyDesc;
		depthHistoryDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthHistoryDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		historySRV.Format = depthHistoryDesc.Format;
		target.depthHistory = std::make_unique<Texture2D>(depthHistoryDesc, (name + " DepthHistory").c_str());
		target.depthHistory->CreateSRV(historySRV);
	}

	// The exposure estimate's own buffers belong to this same lifetime, so no path can draw without them.
	a_work.exposurePartials = std::make_unique<Buffer>(Impl::ExposureBufferDesc(kMaxExposureGroups), nullptr, "NeuralRendering::ExposurePartials");
	a_work.exposurePartials->CreateSRV(Impl::StructuredSRVDesc(kMaxExposureGroups));
	a_work.exposurePartials->CreateUAV(Impl::StructuredUAVDesc(kMaxExposureGroups));
	const std::array<float, 2> initialExposure{ 0.0f, 1.0f };
	D3D11_SUBRESOURCE_DATA initialExposureData{ initialExposure.data(), 0, 0 };
	a_work.exposureState = std::make_unique<Buffer>(Impl::ExposureBufferDesc(2), &initialExposureData, "NeuralRendering::ExposureState");
	a_work.exposureState->CreateSRV(Impl::StructuredSRVDesc(2));
	a_work.exposureState->CreateUAV(Impl::StructuredUAVDesc(2));

	a_work.source.copy_from(a_color);
	a_work.width = a_eyeWidth;
	a_work.height = a_eyeHeight;
	a_work.guideWidth = a_eyeWidth;
	a_work.guideHeight = a_eyeHeight;
	a_work.eyeCount = a_eyeCount;
	a_work.format = a_desc.Format;
	a_work.lastFrame = UINT32_MAX;
	a_work.deviceGeneration = deviceGeneration;
	logger::debug("[NeuralRendering] pass resources {}x{} per eye, {} eye(s), DXGI format {}", a_eyeWidth, a_eyeHeight, a_eyeCount,
		static_cast<uint32_t>(a_desc.Format));
}

nlohmann::json NR::NeuralRendering::Status()
{
	const auto current = outcome.load();
	const auto display = CurrentDisplayState();
	auto runtime = runtimeLayer.Status();
	std::string text;
	switch (display) {
	case DisplayState::kOff:
		text = "Off";
		break;
	case DisplayState::kStarting:
		text = "Starting...";
		break;
	case DisplayState::kActive:
		text = std::format("Active (runtime {})", runtime.value("version", std::string{}));
		break;
	case DisplayState::kNoGpu:
		text = "Requires an NVIDIA RTX GPU";
		break;
	case DisplayState::kDllMissing:
	case DisplayState::kVersionRejected:
	case DisplayState::kFailed:
		text = runtime.value("lastError", std::string{});
		if (text.empty())
			text = std::format("Neural rendering failed ({})", runtime.value("failure", std::string{}));
		break;
	}
	{
		std::scoped_lock lock(failureMutex);
		if (!failureText.empty())
			text = failureText;
	}
	const auto contract = tuning.ProxyContractValue();
	const auto source = static_cast<ExposureSource>(resolvedExposureSource.load());
	return nlohmann::json{
		{ "enabled", globals::features::upscaling.settings.neuralRenderingEnabled },
		{ "adapterSupported", AdapterSupported() },
		{ "outcome", std::string(OutcomeName(current)) },
		{ "displayState", std::string(DisplayStateName(display)) },
		{ "status", text },
		{ "failed", latched.load() },
		{ "canRetry", retryPermitted.load() },
		{ "width", appliedWidth.load() },
		{ "height", appliedHeight.load() },
		{ "eyes", appliedEyes.load() },
		{ "ngxResult", { ngxResult[0].load(), ngxResult[1].load() } },
		{ "lastAppliedFrame", lastAppliedFrame.load() },
		{ "appliedFrames", appliedFrames.load() },
		{ "exposure", exposure.load() },
		{ "exposureSource", source == ExposureSource::kScene ? "Scene" :
							source == ExposureSource::kLocal ? "Local" :
															   "Scalar" },
		{ "proxyContract", ProxyIsHdr(contract) ? "Hdr" : "Sdr" },
		{ "runtime", runtime },
	};
}
