#include "NeuralRendering.h"

#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "NeuralRendering/D3D12Interop.h"
#include "NeuralRendering/Lifecycle.h"
#include "NeuralRendering/Runtime.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/LazyShader.h"

#define I18N_KEY_PREFIX "feature.upscaling.neural_rendering."

struct NeuralRendering::Impl
{
	NR::D3D12Interop interop;
	NR::Runtime runtime;
	struct Eye
	{
		std::unique_ptr<WrappedResource> color, depth, motion, output;
		NR::FrameParameters frame;
		std::unique_ptr<Texture2D> resolved, toneData;
		DirectX::SimpleMath::Vector3 position{}, forward{};
	};
	std::array<Eye, 2> eyes;
	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> isolated;
	Util::LazyShader<ID3D11ComputeShader> prepareColor, prepareToneData, compositeColor;
	struct alignas(16) ColorTransferData
	{
		uint32_t width, height, eyeOffsetX, hasExposure = 0;
		uint32_t conversionMode = 0, exposureMode = 0, compositeMode = 0, maskMode = 0;
		uint32_t visualMode = 0;
		float exposureCompensation = 1.0f, exposureMin = 1.0f, exposureMax = 1.0f, manualExposure = 1.0f;
		float differenceStrength = 1.0f, splitPosition = 0.5f;
		float dynamicRangePadding = 0.0f;
		float4 dynamicRangeProtect{};
		float toneLowStrength = 1.0f, toneRadius = 1.0f, toneHighStrength = 1.0f;
		uint32_t hasToneData = 0;
	};
	static_assert(offsetof(ColorTransferData, dynamicRangeProtect) == 64);
	static_assert(offsetof(ColorTransferData, toneLowStrength) == 80);
	static_assert(offsetof(ColorTransferData, toneRadius) == 84);
	static_assert(offsetof(ColorTransferData, toneHighStrength) == 88);
	static_assert(offsetof(ColorTransferData, hasToneData) == 92);
	static_assert(sizeof(ColorTransferData) == 96);
	std::unique_ptr<ConstantBuffer> colorBuffer;
	std::unique_ptr<Texture2D> original;
	std::unique_ptr<ConstantBuffer> encodeBuffer;
	std::array<std::unique_ptr<Texture2D>, 2> encodeMasks;
	winrt::com_ptr<ID3D11Texture2D> source;
	uint32_t width = 0, height = 0, guideWidth = 0, guideHeight = 0, eyeCount = 0, lastFrame = UINT32_MAX;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool ready = false, failed = false;
	uint32_t appliedFrames = 0;
	std::array<uint32_t, 2> lastNgxResult{};
	uint32_t lastDiagnosticOptions = 0;
	uint32_t debugOptions = 0;
	NR::Diagnostics::ColorConversion conversionMode = NR::Diagnostics::ColorConversion::Production;
	NR::Diagnostics::ExposureMode exposureMode = NR::Diagnostics::ExposureMode::Production;
	NR::Diagnostics::CompositeMode compositeMode = NR::Diagnostics::CompositeMode::Production;
	NR::Diagnostics::VisualMode visualMode = NR::Diagnostics::VisualMode::None;
	float manualExposure = 1.0f, differenceStrength = 1.0f, splitPosition = 0.5f;
	float shadowProtect = 0.0f, highlightProtect = 0.0f;
	float toneLowStrength = 1.0f, toneRadius = 1.0f, toneHighStrength = 1.0f;
	bool useResolutionMotionScale = true;
	NR::Diagnostics* captureDiagnostics = nullptr;
	uint32_t captureFrame = UINT32_MAX;

	~Impl()
	{
		// Static destruction, where the interop wait can block on a GPU that is already gone.
		if (NR::processTerminating.load(std::memory_order_relaxed))
			return;
		try {
			interop.Drain();
		} catch (...) {
			logger::warn("[NeuralRendering] Device unavailable during resource retirement");
		}
	}

	void Initialize()
	{
		interop.Initialize();
		winrt::com_ptr<ID3D11Device1> device;
		winrt::check_hresult(globals::d3d::device->QueryInterface(device.put()));
		winrt::check_hresult(globals::d3d::context->QueryInterface(context.put()));
		const auto level = globals::d3d::device->GetFeatureLevel();
		winrt::check_hresult(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
			__uuidof(ID3D11Device), nullptr, isolated.put()));
		Util::SetResourceName(isolated.get(), "NeuralRendering::ContextState");
		encodeBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Upscaling::UpscalingDataCB>(), "NeuralRendering::Encode CB");
		colorBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorTransferData>(), "NeuralRendering::ColorTransfer CB");
		runtime.Initialize(interop.Device(), Util::PathHelpers::SafeAbsolute(Upscaling::streamline.pluginDir));
		ready = true;
	}

	/**
	 * @brief Releases the per-frame pass resources, keeping the D3D12 device and NGX instance.
	 *        Rebuilding those costs a driver-level re-init; the textures do not.
	 */
	void ReleasePassResources()
	{
		interop.Drain();
		runtime.ResetFeatures();
		eyes = {};
		original.reset();
		encodeMasks = {};
		width = height = guideWidth = guideHeight = eyeCount = 0;
		format = DXGI_FORMAT_UNKNOWN;
		lastFrame = UINT32_MAX;
	}

	/** @brief True while pass resources for a render size exist and can be released. */
	bool HasPassResources() const { return eyeCount != 0; }

	void EnsureResources(ID3D11Texture2D* color, uint32_t w, uint32_t h, uint32_t gw, uint32_t gh, uint32_t count, DXGI_FORMAT colorFormat, bool force)
	{
		if (!force && width == w && height == h && guideWidth == gw && guideHeight == gh && eyeCount == count && format == colorFormat) {
			source.copy_from(color);
			return;
		}
		ReleasePassResources();
		D3D11_TEXTURE2D_DESC maskDesc{};
		maskDesc.Width = gw;
		maskDesc.Height = gh;
		maskDesc.Format = DXGI_FORMAT_R8_UNORM;
		maskDesc.MipLevels = maskDesc.ArraySize = maskDesc.SampleDesc.Count = 1;
		maskDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC maskUAV{};
		maskUAV.Format = maskDesc.Format;
		maskUAV.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		for (uint32_t i = 0; i < encodeMasks.size(); ++i) {
			encodeMasks[i] = std::make_unique<Texture2D>(maskDesc, std::format("NeuralRendering::EncodeMask{}", i).c_str());
			encodeMasks[i]->CreateUAV(maskUAV);
		}
		D3D11_TEXTURE2D_DESC colorDesc = maskDesc;
		colorDesc.Width = w * count;
		colorDesc.Height = h;
		colorDesc.Format = colorFormat;
		colorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = colorFormat;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		original = std::make_unique<Texture2D>(colorDesc, "NeuralRendering::OriginalHDR");
		original->CreateSRV(srv);
		colorDesc.Width = w;
		colorDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC colorUAV = maskUAV;
		colorUAV.Format = colorFormat;
		for (uint32_t i = 0; i < count; ++i) {
			auto& eye = eyes[i];
			const auto name = std::format("NeuralRendering::Eye{}", i);
			eye.resolved = std::make_unique<Texture2D>(colorDesc, (name + " ResolvedHDR").c_str());
			eye.resolved->CreateUAV(colorUAV);
			eye.color = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " HDRInput");
			eye.depth = interop.CreateTexture(gw, gh, DXGI_FORMAT_R32_FLOAT, name + " Depth");
			eye.motion = interop.CreateTexture(gw, gh, DXGI_FORMAT_R16G16_FLOAT, name + " Motion");
			eye.output = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " HDROutput");
		}
		source.copy_from(color);
		width = w;
		height = h;
		guideWidth = gw;
		guideHeight = gh;
		logger::debug("[NeuralRendering] Render-resolution NR {}x{}, guides {}x{}, eyes {}", w, h, gw, gh, count);
		eyeCount = count;
		format = colorFormat;
	}

	uint32_t UpdateFrame(uint32_t i, uint32_t reset, NR::Diagnostics::Frame& diagnostic)
	{
		auto& eye = eyes[i];
		auto& cached = globals::game::frameBufferCached;
		const auto inverseView = cached.GetCameraViewInverse(i).Transpose();
		const auto projection = cached.GetCameraProjUnjittered(i).Transpose();
		const auto adjusted = cached.GetCameraPosAdjust(i);
		const DirectX::SimpleMath::Vector3 position{ adjusted.x, adjusted.y, adjusted.z };
		const DirectX::SimpleMath::Vector3 forward{ inverseView._31, inverseView._32, inverseView._33 };
		auto& sample = diagnostic.camera[i];
		const auto enginePrevious = cached.GetCameraPreviousPosAdjust(i);
		sample.position = { position.x, position.y, position.z };
		sample.previous = { eye.position.x, eye.position.y, eye.position.z };
		sample.enginePrevious = { enginePrevious.x, enginePrevious.y, enginePrevious.z };
		sample.viewTranslation = { inverseView._41, inverseView._42, inverseView._43 };
		sample.distance = (position - eye.position).Length();
		sample.directionDot = forward.Dot(eye.forward);
		sample.projectionDelta = std::max(std::abs(projection._11 - eye.frame.viewToClip._11), std::abs(projection._22 - eye.frame.viewToClip._22));
		uint32_t cameraReset = 0;
		if ((position - eye.position).LengthSquared() > NR::kCameraCutDistance * NR::kCameraCutDistance)
			cameraReset |= NR::Diagnostics::CameraPosition;
		if (forward.Dot(eye.forward) < NR::kCameraCutDirectionDot)
			cameraReset |= NR::Diagnostics::CameraDirection;
		if (std::abs(projection._11 - eye.frame.viewToClip._11) > NR::kProjectionCutThreshold ||
			std::abs(projection._22 - eye.frame.viewToClip._22) > NR::kProjectionCutThreshold)
			cameraReset |= NR::Diagnostics::Projection;
		sample.detected = cameraReset;
		if (diagnostic.options & NR::Diagnostics::ApplyCameraCuts) {
			if (diagnostic.options & NR::Diagnostics::IgnorePosition)
				cameraReset &= ~NR::Diagnostics::CameraPosition;
			reset |= cameraReset;
		}
		if (diagnostic.options & NR::Diagnostics::ForceReset)
			reset |= NR::Diagnostics::Requested;
		eye.frame.reset = reset != 0;
		eye.position = position;
		eye.forward = forward;
		eye.frame.worldToView = inverseView.Invert();
		eye.frame.viewToClip = projection;
		const auto jitter = globals::features::upscaling.jitter;
		eye.frame.jitterX = -jitter.x;
		eye.frame.jitterY = -jitter.y;
		eye.frame.frameTimeMs = *globals::game::deltaTime * 1000.0f;
		eye.frame.feedCameraData = (diagnostic.options & NR::Diagnostics::FeedCameraData) != 0;
		if (diagnostic.options & NR::Diagnostics::ZeroJitter)
			eye.frame.jitterX = eye.frame.jitterY = 0;
		sample.jitterX = eye.frame.jitterX;
		sample.jitterY = eye.frame.jitterY;
		sample.frameTimeMs = eye.frame.frameTimeMs;
		return reset;
	}

	void Transition(ID3D12GraphicsCommandList* commands, Eye& eye, bool enter)
	{
		ID3D12Resource* resources[]{ eye.color->resource.get(), eye.depth->resource.get(), eye.motion->resource.get(), eye.output->resource.get() };
		D3D12_RESOURCE_BARRIER barriers[4]{};
		for (uint32_t i = 0; i < 4; ++i) {
			const auto state = i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[i].Transition = { resources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				enter ? D3D12_RESOURCE_STATE_COMMON : state, enter ? state : D3D12_RESOURCE_STATE_COMMON };
		}
		commands->ResourceBarrier(4, barriers);
	}

	bool NeedsToneData() const
	{
		const bool showBands = visualMode == NR::Diagnostics::VisualMode::ToneLow || visualMode == NR::Diagnostics::VisualMode::ToneHigh;
		const bool useTone = compositeMode == NR::Diagnostics::CompositeMode::Production || visualMode == NR::Diagnostics::VisualMode::ToneLowGain;
		return toneRadius > 0.01f && (showBands || (useTone && toneLowStrength != toneHighStrength));
	}

	void PrepareToneData(uint32_t i)
	{
		if (!NeedsToneData())
			return;
		CS_GPU_PASS("Upscaling::NRPrepareTone");
		auto* shader = prepareToneData.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "PrepareToneData", "NeuralRendering::PrepareToneData CS");
		if (!shader)
			throw std::runtime_error("NR tone-data shader unavailable");
		auto& eye = eyes[i];
		if (!eye.toneData) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R32G32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto texture = std::make_unique<Texture2D>(desc, std::format("NeuralRendering::Eye{} ToneData", i).c_str());
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			texture->CreateSRV(srv);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			texture->CreateUAV(uav);
			eye.toneData = std::move(texture);
		}
		context->ClearState();
		ColorTransferData data{ width, height, i * width };
		colorBuffer->Update(data);
		auto buffer = colorBuffer->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
		ID3D11ShaderResourceView* inputs[]{ nullptr, eye.color->srv, eye.output->srv };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		auto* output = eye.toneData->uav.get();
		context->CSSetUnorderedAccessViews(2, 1, &output, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	void TransferColor(uint32_t i, bool prepare)
	{
		CS_GPU_PASS_SELECT(prepare, "Upscaling::NRPrepare", "Upscaling::NRComposite");
		context->ClearState();
		auto& eye = eyes[i];
		ColorTransferData data{ width, height, i * width };
		data.conversionMode = static_cast<uint32_t>((debugOptions & NR::Diagnostics::DisableColorTransform) ? NR::Diagnostics::ColorConversion::Raw : conversionMode);
		data.exposureMode = static_cast<uint32_t>((debugOptions & NR::Diagnostics::DisableExposure) ? NR::Diagnostics::ExposureMode::Ignore : exposureMode);
		data.compositeMode = static_cast<uint32_t>(compositeMode);
		data.visualMode = static_cast<uint32_t>(visualMode);
		if (debugOptions & (NR::Diagnostics::VisualizeMask | NR::Diagnostics::VisualizeSkinMask | NR::Diagnostics::VisualizeAutoMask))
			data.visualMode = static_cast<uint32_t>(NR::Diagnostics::VisualMode::Mask);
		data.manualExposure = manualExposure;
		data.differenceStrength = differenceStrength;
		data.splitPosition = splitPosition;
		data.dynamicRangeProtect = float4{ shadowProtect, highlightProtect, 0.0f, 0.0f };
		data.toneLowStrength = toneLowStrength;
		data.toneRadius = toneRadius;
		data.toneHighStrength = toneHighStrength;
		data.hasToneData = !prepare && NeedsToneData();
		if (debugOptions & NR::Diagnostics::ForceMaskZero)
			data.maskMode = static_cast<uint32_t>(NR::Diagnostics::MaskMode::ForceZero);
		else if (debugOptions & NR::Diagnostics::ForceMaskOne)
			data.maskMode = static_cast<uint32_t>(NR::Diagnostics::MaskMode::ForceOne);
		else if (debugOptions & NR::Diagnostics::BypassMask)
			data.maskMode = static_cast<uint32_t>(NR::Diagnostics::MaskMode::ForceOne);
		ID3D11ShaderResourceView* exposure = nullptr;
		Feature::SceneExposure sceneExposure;
		if (prepare && Feature::FindSceneExposure(sceneExposure)) {
			exposure = sceneExposure.adaptedLuminance;
			if (captureDiagnostics && i == 0)
				captureDiagnostics->CaptureView("NR_exposure", exposure, captureFrame);
			data.hasExposure = exposure != nullptr;
			data.exposureCompensation = sceneExposure.compensationScale;
			data.exposureMin = sceneExposure.luminanceRange.x;
			data.exposureMax = sceneExposure.luminanceRange.y;
			if (!std::isfinite(data.exposureCompensation) || !std::isfinite(data.exposureMin) || !std::isfinite(data.exposureMax))
				data.hasExposure = 0;
		}
		colorBuffer->Update(data);
		auto buffer = colorBuffer->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
		globals::state->BindSharedDataCS(context.get(), true);
		ID3D11ShaderResourceView* inputs[]{ original->srv.get(), prepare ? nullptr : eye.color->srv,
			prepare ? nullptr : eye.output->srv, exposure,
			data.hasToneData ? eye.toneData->srv.get() : nullptr };
		ID3D11UnorderedAccessView* outputs[]{ prepare ? eye.color->uav : eye.resolved->uav.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(prepare ? prepareColor.get() : compositeColor.get(), nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	bool Draw(ID3D11Texture2D* color, ID3D11ShaderResourceView* const* inputs, ID3D11ComputeShader* shader, uint32_t reset, const NR::Tuning& tuning, NR::Diagnostics::Frame& diagnostic, NR::Diagnostics& diagnostics)
	{
		CS_GPU_PASS("Upscaling::NeuralRendering");
		captureDiagnostics = &diagnostics;
		captureFrame = diagnostic.number;
		struct ContextScope
		{
			ID3D11DeviceContext1* context;
			winrt::com_ptr<ID3DDeviceContextState> previous;
			ContextScope(ID3D11DeviceContext1* ctx, ID3DDeviceContextState* isolated) :
				context(ctx)
			{
				context->SwapDeviceContextState(isolated, previous.put());
				context->ClearState();
			}
			~ContextScope()
			{
				context->ClearState();
				context->SwapDeviceContextState(previous.get(), nullptr);
			}
		} scope(context.get(), isolated.get());
		const D3D11_BOX originalBox{ 0, 0, 0, width * eyeCount, height, 1 };
		context->CopySubresourceRegion(original->resource.get(), 0, 0, 0, 0, color, 0, &originalBox);
		const bool capture = diagnostics.BeginCapture(diagnostic.number);
		if (capture)
			diagnostics.DumpTexture("00_original_scene", original->resource.get(), diagnostic.number);
		if (capture) {
			diagnostics.CaptureView("NR_depth", inputs[3], diagnostic.number);
			diagnostics.CaptureView("NR_motion", inputs[2], diagnostic.number);
		}
		for (uint32_t i = 0; i < eyeCount; ++i)
			TransferColor(i, true);
		if (capture)
			diagnostics.DumpTexture("01_input", eyes[0].color->resource11, diagnostic.number);
		if (debugOptions & NR::Diagnostics::BypassEvaluation) {
			diagnostic.outcome = NR::Diagnostics::Outcome::Bypassed;
			diagnostics.FinishCapture(diagnostic.number);
			return true;
		}
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetShaderResources(0, 4, inputs);
		globals::state->BindSharedDataCS(context.get(), true);
		{
			CS_GPU_PASS("Upscaling::NREncodeGuides");
			for (uint32_t i = 0; i < eyeCount; ++i) {
				auto& eye = eyes[i];
				diagnostic.reset[i] = UpdateFrame(i, reset, diagnostic);
				Upscaling::UpscalingDataCB data{ { float(guideWidth), float(guideHeight) }, i * guideWidth, 0 };
				encodeBuffer->Update(data);
				auto buffer = encodeBuffer->CB();
				context->CSSetConstantBuffers(0, 1, &buffer);
				// The shared encoder writes both masks even though NR only consumes motion and depth.
				ID3D11UnorderedAccessView* outputs[]{ encodeMasks[0]->uav.get(), encodeMasks[1]->uav.get(),
					eye.motion->uav, eye.depth->uav };
				context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
				context->Dispatch((guideWidth + 7) / 8, (guideHeight + 7) / 8, 1);
				if (eye.frame.reset || (diagnostic.options & NR::Diagnostics::ZeroMotion)) {
					constexpr float zero[4]{};
					context->ClearUnorderedAccessViewFloat(eye.motion->uav, zero);
				}
			}
		}
		context->ClearState();
		// Resetting persistent NR history must not overlap its prior GPU evaluation.
		for (uint32_t i = 0; i < eyeCount; ++i) {
			if (eyes[i].frame.reset) {
				interop.Drain();
				break;
			}
		}
		bool success = true;
		{
			CS_GPU_PASS("Upscaling::NREvaluate");
			auto* commands = interop.Begin();
			for (uint32_t i = 0; i < eyeCount && success; ++i) {
				auto& eye = eyes[i];
				Transition(commands, eye, true);
				if (debugOptions & (NR::Diagnostics::InteropRoundTrip | NR::Diagnostics::CopyInputToOutput)) {
					D3D12_RESOURCE_BARRIER copyBarriers[2]{};
					copyBarriers[0].Type = copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					copyBarriers[0].Transition = { eye.color->resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
						D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE };
					copyBarriers[1].Transition = { eye.output->resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST };
					commands->ResourceBarrier(2, copyBarriers);
					commands->CopyResource(eye.output->resource.get(), eye.color->resource.get());
					copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
					copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
					copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
					copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
					commands->ResourceBarrier(2, copyBarriers);
				} else {
					// The encoder extracts render-resolution guides into zero-origin per-eye textures.
					NR::GuideParameters guides;
					guides.depth = { 0, 0, guideWidth, guideHeight };
					guides.motion = { 0, 0, guideWidth, guideHeight };
					// MotionBlur produces normalized eye-UV displacement; NR consumes input-pixel displacement.
					guides.motionScaleX = useResolutionMotionScale ? static_cast<float>(width) : 1.0f;
					guides.motionScaleY = useResolutionMotionScale ? static_cast<float>(height) : 1.0f;
					success = runtime.Evaluate(commands, i, eye.color->resource.get(), eye.depth->resource.get(),
						eye.motion->resource.get(), eye.output->resource.get(), width, height, guides, eye.frame, tuning);
				}
				diagnostic.result[i] = eye.frame.result;
				lastNgxResult[i] = eye.frame.result;
				if (success)
					diagnostic.evaluated |= 1u << i;
				if (eye.frame.created) {
					diagnostic.created |= 1u << i;
					diagnostic.reset[i] |= NR::Diagnostics::FeatureCreated;
				}
				Transition(commands, eye, false);
			}
			interop.End();
		}
		if (diagnostic.options & NR::Diagnostics::SerializeGPU)
			interop.Drain();
		diagnostic.submittedFence = interop.SubmittedFence();
		diagnostic.completedFence = interop.CompletedFence();
		if (!success) {
			diagnostics.FinishCapture(diagnostic.number);
			return false;
		}
		if (capture) {
			interop.Drain();
			diagnostics.DumpTexture("02_output", eyes[0].output->resource11, diagnostic.number);
		}
		if (diagnostic.options & NR::Diagnostics::BypassWriteback) {
			diagnostics.FinishCapture(diagnostic.number);
			return true;
		}
		for (uint32_t i = 0; i < eyeCount; ++i) {
			PrepareToneData(i);
			TransferColor(i, false);
		}
		if (capture) {
			diagnostics.DumpTexture("03_pre_composite", original->resource.get(), diagnostic.number);
		}
		const D3D11_BOX box{ 0, 0, 0, width, height, 1 };
		for (uint32_t i = 0; i < eyeCount; ++i) {
			context->CopySubresourceRegion(color, 0, i * width, 0, 0, eyes[i].resolved->resource.get(), 0, &box);
			diagnostic.copied |= 1u << i;
		}
		if (capture)
			diagnostics.DumpTexture("04_post_composite", color, diagnostic.number);
		++appliedFrames;
		diagnostics.FinishCapture(diagnostic.number);
		captureDiagnostics = nullptr;
		return true;
	}
};

NeuralRendering::NeuralRendering() :
	impl(std::make_unique<Impl>()) {}
NeuralRendering::~NeuralRendering() = default;

void NeuralRendering::SetupResources() { retryRequested = recreate = resetHistory = true; }
void NeuralRendering::ResetHistory() { resetHistory = true; }
void NeuralRendering::ClearShaderCache() { retryRequested = clearShaders = resetHistory = true; }

void NeuralRendering::Reset(bool enabled)
{
	diagnostics.SetDeveloperMode(globals::state->IsDeveloperMode());
	if (enabled)
		diagnostics.EndFrame(globals::state->frameCount, enabled, globals::state->worldRenderedThisFrame, globals::state->IsPausedOrMenuOpen(globals::game::ui));
	if (!enabled)
		retryRequested = true;
	if (!enabled || !globals::state->worldRenderedThisFrame)
		resetHistory = true;
}

void NeuralRendering::PublishStatus(Status::State state, std::string text)
{
	Status next;
	next.state = state;
	next.text = std::move(text);
	next.failed = state == Status::State::kFailed;
	next.runtimeVersion = impl->runtime.Version();
	next.width = impl->width;
	next.height = impl->height;
	next.eyes = impl->eyeCount;
	next.appliedFrames = impl->appliedFrames;
	next.ngxResult = impl->lastNgxResult;
	std::scoped_lock lock(statusMutex);
	status = std::move(next);
	publishedState.store(state, std::memory_order_relaxed);
}

void NeuralRendering::PublishFailure(const std::string& detail)
{
	PublishStatus(Status::State::kFailed, std::format("{} {}", T(TKEY("status_failed"), "Neural Rendering stopped:"), detail));
}

NeuralRendering::Status NeuralRendering::GetStatus() const
{
	std::scoped_lock lock(statusMutex);
	auto snapshot = status;
	if (snapshot.text.empty())
		snapshot.text = T(TKEY("status_off"), "Off");
	snapshot.lastAppliedFrame = appliedFrame.load(std::memory_order_relaxed);
	return snapshot;
}

void NeuralRendering::DrawSettings(bool& enabled, NR::Tuning& tuning)
{
	ImGui::PushID("NeuralRendering");
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable Neural Rendering"), &enabled))
		retryRequested = resetHistory = true;
	ImGui::TextWrapped("%s", T(TKEY("description"),
								 "One display-referred NR proxy pass at eye render resolution, composed back into scene-linear HDR before DLSS/FSR and frame-generation capture. Keep Reset NR every frame off for normal use. Requires an NR-capable NVIDIA GPU and the 310.8.x runtime."));
	int style = static_cast<int>(std::min(tuning.style, NR::Tuning::kMaxStyle));
	const std::array<const char*, NR::Tuning::kMaxStyle + 1> styleLabels{
		T(TKEY("style_0"), "Style 0"),
		T(TKEY("style_1"), "Style 1"),
		T(TKEY("style_2"), "Style 2"),
	};
	bool changed = ImGui::Combo(T(TKEY("style"), "Style"), &style, styleLabels.data(), static_cast<int>(styleLabels.size()));
	bool recreateTuning = changed;
	if (changed)
		tuning.style = static_cast<uint32_t>(style);
	changed |= ImGui::SliderFloat(T(TKEY("intensity"), "Intensity"), &tuning.intensity, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("local_tone"), "Local Tone Strength"), &tuning.localToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("local_structure"), "Local Structure Strength"), &tuning.localStructureStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("skin_structure"), "Skin Structure Strength"), &tuning.skinStructureStrength, NR::Tuning::kAutomaticSkinStructure, NR::Tuning::kMaxStrength,
		tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure ? T(TKEY("skin_auto"), "Auto") : "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	const bool autoMaskChanged = ImGui::Checkbox(T(TKEY("use_auto_mask"), "Use Auto Mask"), &tuning.useAutoMask);
	changed |= autoMaskChanged;
	recreateTuning |= autoMaskChanged;
	if (ImGui::Button(T(TKEY("restore_defaults"), "Restore NR Defaults"))) {
		tuning = {};
		changed = recreateTuning = true;
	}
	if (changed)
		tuning.Sanitize();
	if (recreateTuning)
		recreate = resetHistory = true;
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("reset_history"), "Reset NR History")))
		resetHistory = true;
	if (enabled && ImGui::Button(T(TKEY("retry"), "Retry NR")))
		RequestRetry();
	if (globals::state->IsDeveloperMode()) {
		if (ImGui::Checkbox("Use resolution-scaled NR motion", &impl->useResolutionMotionScale))
			resetHistory = true;
		diagnostics.DrawSettings();
	}
	const auto current = GetStatus();
	ImGui::TextWrapped("%s", current.text.c_str());
	ImGui::PopID();
}

void NeuralRendering::DrawDiagnosticsOverlay()
{
	diagnostics.DrawOverlay(GetStatus().text);
}

void NeuralRendering::RecordStage(bool finishedPost)
{
	auto* main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
	diagnostics.Stage(globals::state->frameCount, finishedPost, reinterpret_cast<uintptr_t>(main));
}

void NeuralRendering::CaptureBeforeUpscaling()
{
	if (!globals::state)
		return;
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!diagnostics.CaptureActive(globals::state->frameCount) && diagnostics.BeginCapture(globals::state->frameCount))
		diagnostics.CaptureStage("00_original_scene", Util::AsReal(main.texture), globals::state->frameCount);
	diagnostics.CaptureStage("05_pre_sr", Util::AsReal(main.texture), globals::state->frameCount);
}

void NeuralRendering::CaptureAfterUpscaling()
{
	if (!globals::state)
		return;
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	diagnostics.CaptureStage("06_post_sr", Util::AsReal(main.texture), globals::state->frameCount);
	diagnostics.FinishCapture(globals::state->frameCount);
}

void NeuralRendering::DrawBeforeUpscaling(bool enabled, const NR::Tuning& tuning, uint32_t target, float2 renderSize)
{
	using Outcome = NR::Diagnostics::Outcome;
	auto& diagnostic = diagnostics.BeginHook(globals::state->frameCount, target);
	const auto action = NR::DecideFrame({ enabled, globals::state->worldRenderedThisFrame, impl->failed,
		retryRequested.load(), impl->ready, impl->lastFrame, globals::state->frameCount });
	if (action == NR::FrameAction::ReleasePassResources) {
		diagnostic.outcome = Outcome::Disabled;
		// A latched failure may be a wedged queue, so its resources wait for Retry's draining teardown.
		if (impl->HasPassResources() && !impl->failed) {
			std::string failure;
			try {
				impl->ReleasePassResources();
			} catch (const winrt::hresult_error& error) {
				failure = winrt::to_string(error.message());
			} catch (const std::exception& error) {
				failure = error.what();
			}
			if (!failure.empty()) {
				diagnostic.outcome = Outcome::Error;
				LatchFailure();
				PublishFailure(failure);
				logger::error("[NeuralRendering] {}", failure);
				retryRequested = resetHistory = true;
				return;
			}
		}
		if (publishedState.load(std::memory_order_relaxed) != Status::State::kOff) {
			logger::debug("[NeuralRendering] Disabled");
			PublishStatus(Status::State::kOff, T(TKEY("status_off"), "Off"));
		}
		retryRequested = resetHistory = true;
		return;
	}
	auto* state = globals::state;
	if (action == NR::FrameAction::SkipNoWorld) {
		diagnostic.outcome = Outcome::NoWorld;
		if (publishedState.load(std::memory_order_relaxed) == Status::State::kOff)
			PublishStatus(Status::State::kStarting, T(TKEY("status_starting"), "Starting..."));
		resetHistory = true;
		return;
	}
	try {
		// Every enabled world frame consumes a queued retry, whichever action it settled on.
		retryRequested = false;
		if (action == NR::FrameAction::RebuildThenRun) {
			// Retire both APIs before releasing a failed runtime and its shared resources.
			impl->interop.Drain();
			impl = std::make_unique<Impl>();
			resetHistory = true;
		}
		auto& work = *impl;
		const auto selected = diagnostics.Selected();
		diagnostic.options = selected.options;
		if (diagnostic.options != work.lastDiagnosticOptions) {
			resetHistory = true;
			if ((diagnostic.options ^ work.lastDiagnosticOptions) & NR::Diagnostics::FeedCameraData) {
				work.interop.Drain();
				work.runtime.ResetFeatures();
			}
			work.lastDiagnosticOptions = diagnostic.options;
		}
		if (action == NR::FrameAction::SkipLatched) {
			diagnostic.outcome = Outcome::FailedLatch;
			return;
		}
		if (action == NR::FrameAction::SkipDuplicate) {
			++diagnostic.duplicates;
			return;
		}
		if (!work.ready) {
			work.Initialize();
			const auto luid = work.interop.AdapterLuid();
			logger::debug("[NeuralRendering] D3D12 device on renderer adapter LUID {:08X}:{:08X}", luid.HighPart, luid.LowPart);
		}
		if (clearShaders.exchange(false)) {
			work.prepareColor.Reset();
			work.prepareToneData.Reset();
			work.compositeColor.Reset();
		}
		auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		auto* color = Util::AsReal(targets[RE::RENDER_TARGETS::kMAIN].texture);
		Upscaling::EncodeInputViews inputs{};
		const char* missingInput = nullptr;
		if (!globals::features::upscaling.GetEncodeInputs(inputs, missingInput))
			throw std::runtime_error(std::format("Missing NR guide input ({})", missingInput));
		const auto count = globals::game::isVR ? 2u : 1u;
		const auto gw = NR::EyeRenderWidth(static_cast<uint32_t>(renderSize.x), count);
		const auto gh = static_cast<uint32_t>(renderSize.y);
		D3D11_TEXTURE2D_DESC desc{};
		if (color)
			color->GetDesc(&desc);
		const auto w = gw;
		const auto h = gh;
		diagnostic.width = w;
		diagnostic.height = h;
		diagnostic.eyeCount = count;
		diagnostic.format = desc.Format;
		diagnostic.proxyFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		diagnostic.source = reinterpret_cast<uintptr_t>(color);
		if (!NR::IsSupportedOutput(desc, w, h, count))
			throw std::runtime_error(std::format("Unsupported NR output: {}x{}, DXGI format {}, array {}, samples {}, guides {}x{}",
				desc.Width, desc.Height, static_cast<uint32_t>(desc.Format), desc.ArraySize, desc.SampleDesc.Count, gw, gh));
		for (auto* input : inputs) {
			D3D11_TEXTURE2D_DESC guide{};
			if (!input || !Util::GetTexture2DDesc(input, guide) || guide.Width < gw * count || guide.Height < gh ||
				guide.ArraySize != 1 || guide.SampleDesc.Count != 1)
				throw std::runtime_error("Missing or incompatible NR guide texture");
		}
		const bool forceRecreate = recreate.exchange(false);
		diagnostic.recreated = forceRecreate || work.width != w || work.height != h || work.guideWidth != gw || work.guideHeight != gh || work.eyeCount != count || work.format != desc.Format;
		work.EnsureResources(color, w, h, gw, gh, count, desc.Format, forceRecreate);
		// The same cached encoder permutations the upscaling path uses: kNONE keeps motion undilated,
		// DLSS dilates it, and kTypedDepth supplies NR's depth guide.
		const bool dilateMotion = (diagnostic.options & NR::Diagnostics::DilateMotion) != 0;
		auto* shader = globals::features::upscaling.GetEncodeTexturesCS(dilateMotion ? Upscaling::UpscaleMethod::kDLSS : Upscaling::UpscaleMethod::kNONE,
			Upscaling::EncodeOutput::kTypedDepth);
		auto* prepare = work.prepareColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "Prepare", "NeuralRendering::PrepareColor CS");
		auto* composite = work.compositeColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "Composite", "NeuralRendering::CompositeHDR CS");
		if (!shader || !prepare || !composite)
			throw std::runtime_error("NR encoder or color-transfer shader unavailable");
		work.debugOptions = diagnostic.options;
		work.conversionMode = selected.conversion;
		work.exposureMode = selected.exposure;
		work.compositeMode = selected.composite;
		work.visualMode = selected.visual;
		work.manualExposure = selected.manualExposure;
		work.differenceStrength = selected.differenceStrength;
		work.splitPosition = selected.splitPosition;
		work.shadowProtect = selected.shadowProtect;
		work.highlightProtect = selected.highlightProtect;
		work.toneRadius = selected.toneRadius;
		uint32_t reset = NR::Diagnostics::FrameResetReasons(resetHistory.exchange(false), work.lastFrame, state->frameCount);
		auto boundedTuning = tuning;
		boundedTuning.Sanitize();
		if (diagnostic.options & NR::Diagnostics::DisableTone)
			boundedTuning.localToneStrength = 0.0f;
		if (diagnostic.options & NR::Diagnostics::DisableStructure)
			boundedTuning.localStructureStrength = 0.0f;
		if (diagnostic.options & NR::Diagnostics::DisableSkin)
			boundedTuning.skinStructureStrength = NR::Tuning::kAutomaticSkinStructure;
		work.toneLowStrength = boundedTuning.localToneStrength;
		work.toneHighStrength = boundedTuning.localStructureStrength;
		diagnostic.conversion = static_cast<uint32_t>(work.conversionMode);
		diagnostic.exposureMode = static_cast<uint32_t>(work.exposureMode);
		diagnostic.compositeMode = static_cast<uint32_t>(work.compositeMode);
		diagnostic.visualMode = static_cast<uint32_t>(work.visualMode);
		diagnostic.manualExposure = work.manualExposure;
		diagnostic.differenceStrength = work.differenceStrength;
		diagnostic.splitPosition = work.splitPosition;
		diagnostic.intensity = boundedTuning.intensity;
		diagnostic.localTone = boundedTuning.localToneStrength;
		diagnostic.localStructure = boundedTuning.localStructureStrength;
		diagnostic.skinStructure = boundedTuning.skinStructureStrength;
		if (!work.Draw(color, inputs.data(), shader, reset, boundedTuning, diagnostic, diagnostics))
			throw std::runtime_error(std::format("the NVIDIA runtime failed to process a frame. Update the GPU driver or the 310.8 runtime, then press Retry (NGX L/R 0x{:08X}/0x{:08X})",
				diagnostic.result[0], diagnostic.result[1]));
		appliedFrame.store(state->frameCount, std::memory_order_relaxed);
		if (publishedState.load(std::memory_order_relaxed) != Status::State::kActive) {
			const auto runtime = work.runtime.Version();
			const auto luid = work.interop.AdapterLuid();
			PublishStatus(Status::State::kActive, std::vformat(T(TKEY("status_active"), "Active (runtime {})"), std::make_format_args(runtime)));
			logger::info("[NeuralRendering] active: runtime {} on adapter LUID {:08X}:{:08X}", runtime, luid.HighPart, luid.LowPart);
		}
		diagnostic.outcome = (diagnostic.options & NR::Diagnostics::BypassWriteback) ? Outcome::Bypassed : Outcome::Applied;
		work.lastFrame = state->frameCount;
	} catch (const winrt::hresult_error& error) {
		diagnostic.outcome = Outcome::Error;
		LatchFailure();
		PublishFailure(winrt::to_string(error.message()));
		logger::error("[NeuralRendering] D3D initialization/dispatch failed: 0x{:08X}", static_cast<uint32_t>(error.code().value));
	} catch (const std::exception& error) {
		diagnostic.outcome = Outcome::Error;
		LatchFailure();
		PublishFailure(error.what());
		logger::error("[NeuralRendering] {}", error.what());
	}
}

void NeuralRendering::LatchFailure()
{
	if (NR::OnFailure(impl->interop.DeviceRemoved()) == NR::FailureAction::TeardownThenLatch) {
		// A removed device cannot be reused: the replacement starts latched, so only Retry rebuilds it.
		impl = std::make_unique<Impl>();
	}
	impl->failed = true;
}

#undef I18N_KEY_PREFIX
