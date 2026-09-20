#include "NeuralRendering.h"

#include "Deferred.h"
#include "Features/PostProcessing.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "NeuralRendering/D3D12Interop.h"
#include "NeuralRendering/Runtime.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/LazyShader.h"

struct NeuralRendering::Impl
{
	NR::D3D12Interop interop;
	NR::Runtime runtime;
	struct Eye
	{
		NR::SharedTexture color, depth, motion, output;
		NR::FrameParameters frame;
		std::unique_ptr<Texture2D> resolved;
		DirectX::SimpleMath::Vector3 position{}, forward{};
	};
	std::array<Eye, 2> eyes;
	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> isolated;
	Util::LazyShader<ID3D11ComputeShader> encode, prepareColor, compositeColor;
	struct alignas(16) ColorTransferData
	{
		uint32_t width, height, eyeOffsetX, hasExposure = 0;
		uint32_t conversionMode = 0, exposureMode = 0, compositeMode = 0, maskMode = 0;
		uint32_t visualMode = 0;
		float exposureCompensation = 1.0f, exposureMin = 1.0f, exposureMax = 1.0f, manualExposure = 1.0f;
		float differenceStrength = 1.0f, splitPosition = 0.5f;
		float4 dynamicRangeProtect{};
		float toneLowStrength = 1.0f, toneRadius = 1.0f, toneHighStrength = 1.0f, tonePadding = 0.0f;
	};
	std::unique_ptr<ConstantBuffer> colorBuffer;
	std::unique_ptr<Texture2D> original, reactive;
	uint32_t maskFrame = UINT32_MAX;
	std::unique_ptr<ConstantBuffer> encodeBuffer;
	std::array<std::unique_ptr<Texture2D>, 2> encodeMasks;
	winrt::com_ptr<ID3D11Texture2D> source;
	uint32_t width = 0, height = 0, guideWidth = 0, guideHeight = 0, eyeCount = 0, lastFrame = UINT32_MAX;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool ready = false, failed = false;
	uint32_t lastDiagnosticOptions = 0;
	uint32_t debugOptions = 0;
	NR::Diagnostics::ColorConversion conversionMode = NR::Diagnostics::ColorConversion::Production;
	NR::Diagnostics::ExposureMode exposureMode = NR::Diagnostics::ExposureMode::Production;
	NR::Diagnostics::CompositeMode compositeMode = NR::Diagnostics::CompositeMode::Production;
	NR::Diagnostics::VisualMode visualMode = NR::Diagnostics::VisualMode::None;
	float manualExposure = 1.0f, differenceStrength = 1.0f, splitPosition = 0.5f;
	float shadowProtect = 0.0f, highlightProtect = 0.0f;
	float toneLowStrength = 1.0f, toneRadius = 1.0f, toneHighStrength = 1.0f;
	NR::Diagnostics* captureDiagnostics = nullptr;
	uint32_t captureFrame = UINT32_MAX;

	~Impl()
	{
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
		runtime.Initialize(interop.Device(), std::filesystem::absolute(Upscaling::streamline.pluginDir));
		ready = true;
	}

	void EnsureResources(ID3D11Texture2D* color, uint32_t w, uint32_t h, uint32_t gw, uint32_t gh, uint32_t count, DXGI_FORMAT colorFormat, bool force)
	{
		if (!force && width == w && height == h && guideWidth == gw && guideHeight == gh && eyeCount == count && format == colorFormat) {
			source.copy_from(color);
			return;
		}
		interop.Drain();
		runtime.ResetFeatures();
		eyes = {};
		lastFrame = maskFrame = UINT32_MAX;
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
		maskDesc.Width = w * count;
		maskDesc.Height = h;
		maskDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
		reactive = std::make_unique<Texture2D>(maskDesc, "NeuralRendering::ReactiveMask");
		srv.Format = maskDesc.Format;
		reactive->CreateSRV(srv);
		reactive->CreateUAV(maskUAV);
		colorDesc.Width = w;
		colorDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC colorUAV = maskUAV;
		colorUAV.Format = colorFormat;
		srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		for (uint32_t i = 0; i < count; ++i) {
			auto& eye = eyes[i];
			const auto name = std::format("NeuralRendering::Eye{}", i);
			eye.resolved = std::make_unique<Texture2D>(colorDesc, (name + " ResolvedHDR").c_str());
			eye.resolved->CreateUAV(colorUAV);
			eye.color = interop.CreateTexture(w, h, srv.Format, name + " HDRInput");
			eye.color.texture->CreateSRV(srv);
			eye.depth = interop.CreateTexture(gw, gh, DXGI_FORMAT_R32_FLOAT, name + " Depth");
			eye.motion = interop.CreateTexture(gw, gh, DXGI_FORMAT_R16G16_FLOAT, name + " Motion");
			eye.output = interop.CreateTexture(w, h, srv.Format, name + " HDROutput");
			eye.output.texture->CreateSRV(srv);
		}
		source.copy_from(color);
		width = w;
		height = h;
		guideWidth = gw;
		guideHeight = gh;
		logger::info("[NeuralRendering] Render-resolution NR {}x{}, guides {}x{}, eyes {}", w, h, gw, gh, count);
		eyeCount = count;
		format = colorFormat;
	}

	uint32_t UpdateFrame(uint32_t i, uint32_t reset, NR::Diagnostics::Frame& diagnostic)
	{
		constexpr float kCameraCutDistance = 256.0f;
		constexpr float kCameraCutDirectionDot = 0.5f;
		constexpr float kProjectionCutThreshold = 0.1f;
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
		if ((position - eye.position).LengthSquared() > kCameraCutDistance * kCameraCutDistance)
			cameraReset |= NR::Diagnostics::CameraPosition;
		if (forward.Dot(eye.forward) < kCameraCutDirectionDot)
			cameraReset |= NR::Diagnostics::CameraDirection;
		if (std::abs(projection._11 - eye.frame.viewToClip._11) > kProjectionCutThreshold ||
			std::abs(projection._22 - eye.frame.viewToClip._22) > kProjectionCutThreshold)
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
		if (diagnostic.options & NR::Diagnostics::ZeroJitter)
			eye.frame.jitterX = eye.frame.jitterY = 0;
		sample.jitterX = eye.frame.jitterX;
		sample.jitterY = eye.frame.jitterY;
		sample.frameTimeMs = eye.frame.frameTimeMs;
		return reset;
	}

	void Transition(ID3D12GraphicsCommandList* commands, Eye& eye, bool enter)
	{
		ID3D12Resource* resources[]{ eye.color.resource.get(), eye.depth.resource.get(), eye.motion.resource.get(), eye.output.resource.get() };
		D3D12_RESOURCE_BARRIER barriers[4]{};
		for (uint32_t i = 0; i < 4; ++i) {
			const auto state = i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[i].Transition = { resources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				enter ? D3D12_RESOURCE_STATE_COMMON : state, enter ? state : D3D12_RESOURCE_STATE_COMMON };
		}
		commands->ResourceBarrier(4, barriers);
	}

	void TransferColor(uint32_t i, bool prepare)
	{
		CS_GPU_PASS_SELECT(prepare, "NeuralRendering::PrepareColor", "NeuralRendering::CompositeHDR");
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
		if (debugOptions & NR::Diagnostics::ForceMaskZero)
			data.maskMode = 1;
		else if (debugOptions & NR::Diagnostics::ForceMaskOne)
			data.maskMode = 2;
		else if (debugOptions & NR::Diagnostics::BypassMask)
			data.maskMode = 2;
		ID3D11ShaderResourceView* exposure = nullptr;
		auto& post = globals::features::postProcessing;
		if (prepare && post.loaded && !post.bypass) {
			auto* adaptation = post.GetPipelineFeature<HistogramAutoExposure>(PostProcessing::FeaturePipelineIndex::AutoExposure);
			if (adaptation && adaptation->enabled) {
				exposure = adaptation->GetAdaptationSRV();
				if (captureDiagnostics && i == 0)
					captureDiagnostics->CaptureView("NR_exposure", exposure, captureFrame);
				data.hasExposure = exposure != nullptr;
				data.exposureCompensation = std::exp2(std::clamp(adaptation->settings.ExposureCompensation, -16.0f, 16.0f));
				data.exposureMin = std::exp2(std::clamp(adaptation->settings.AdaptationRange.x - 3.0f, -16.0f, 16.0f));
				data.exposureMax = std::max(data.exposureMin, std::exp2(std::clamp(adaptation->settings.AdaptationRange.y - 3.0f, -16.0f, 16.0f)));
				if (!std::isfinite(data.exposureCompensation) || !std::isfinite(data.exposureMin) || !std::isfinite(data.exposureMax))
					data.hasExposure = 0;
			}
			auto* grading = post.GetPipelineFeature<ColorGrading>(PostProcessing::FeaturePipelineIndex::ColorGrading);
			if (grading && grading->enabled && std::isfinite(grading->settings.exposureTemperatureTint.x))
				data.manualExposure = std::clamp(grading->settings.exposureTemperatureTint.x, 1e-4f, 1e4f);
		}
		colorBuffer->Update(data);
		auto buffer = colorBuffer->CB();
		auto shared = globals::state->sharedDataCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
		context->CSSetConstantBuffers(5, 1, &shared);
		ID3D11ShaderResourceView* inputs[]{ original->srv.get(), prepare ? nullptr : eye.color.texture->srv.get(),
			prepare ? nullptr : eye.output.texture->srv.get(), exposure };
		ID3D11UnorderedAccessView* outputs[]{ prepare ? eye.color.texture->uav.get() : eye.resolved->uav.get(),
			prepare ? nullptr : reactive->uav.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(prepare ? prepareColor.get() : compositeColor.get(), nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	bool Draw(ID3D11Texture2D* color, ID3D11ShaderResourceView* const* inputs, ID3D11ComputeShader* shader, uint32_t reset, const NR::Tuning& tuning, NR::Diagnostics::Frame& diagnostic, NR::Diagnostics& diagnostics)
	{
		CS_GPU_PASS("NeuralRendering::Evaluate");
		captureDiagnostics = &diagnostics;
		captureFrame = diagnostic.number;
		struct ContextScope
		{
			ID3D11DeviceContext1* context;
			winrt::com_ptr<ID3DDeviceContextState> previous;
			ContextScope(ID3D11DeviceContext1* ctx, ID3DDeviceContextState* isolated) : context(ctx)
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
		maskFrame = UINT32_MAX;
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
			diagnostics.DumpTexture("01_input", eyes[0].color.texture->resource.get(), diagnostic.number);
		if (debugOptions & NR::Diagnostics::BypassEvaluation) {
			diagnostic.outcome = NR::Diagnostics::Outcome::Bypassed;
			diagnostics.FinishCapture(diagnostic.number);
			return true;
		}
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetShaderResources(0, 4, inputs);
		auto shared = globals::state->sharedDataCB->CB();
		context->CSSetConstantBuffers(5, 1, &shared);
		for (uint32_t i = 0; i < eyeCount; ++i) {
			auto& eye = eyes[i];
			diagnostic.reset[i] = UpdateFrame(i, reset, diagnostic);
			Upscaling::UpscalingDataCB data{ { float(guideWidth), float(guideHeight) }, i * guideWidth, 0 };
			encodeBuffer->Update(data);
			auto buffer = encodeBuffer->CB();
			context->CSSetConstantBuffers(0, 1, &buffer);
			// The shared encoder writes both masks even though NR only consumes motion and depth.
			ID3D11UnorderedAccessView* outputs[]{ encodeMasks[0]->uav.get(), encodeMasks[1]->uav.get(),
				eye.motion.texture->uav.get(), eye.depth.texture->uav.get() };
			context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
			context->Dispatch((guideWidth + 7) / 8, (guideHeight + 7) / 8, 1);
			if (eye.frame.reset || (diagnostic.options & NR::Diagnostics::ZeroMotion)) {
				constexpr float zero[4]{};
				context->ClearUnorderedAccessViewFloat(eye.motion.texture->uav.get(), zero);
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
		auto* commands = interop.Begin();
		bool success = true;
		for (uint32_t i = 0; i < eyeCount && success; ++i) {
			auto& eye = eyes[i];
			Transition(commands, eye, true);
			if (debugOptions & (NR::Diagnostics::InteropRoundTrip | NR::Diagnostics::CopyInputToOutput)) {
				D3D12_RESOURCE_BARRIER copyBarriers[2]{};
				copyBarriers[0].Type = copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				copyBarriers[0].Transition = { eye.color.resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE };
				copyBarriers[1].Transition = { eye.output.resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST };
				commands->ResourceBarrier(2, copyBarriers);
				commands->CopyResource(eye.output.resource.get(), eye.color.resource.get());
				copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
				copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				commands->ResourceBarrier(2, copyBarriers);
			} else
				success = runtime.Evaluate(commands, i, eye.color.resource.get(), eye.depth.resource.get(),
					eye.motion.resource.get(), eye.output.resource.get(), width, height, guideWidth, guideHeight, eye.frame, tuning);
			diagnostic.result[i] = eye.frame.result;
			if (success)
				diagnostic.evaluated |= 1u << i;
			if (eye.frame.created) {
				diagnostic.created |= 1u << i;
				diagnostic.reset[i] |= NR::Diagnostics::FeatureCreated;
			}
			Transition(commands, eye, false);
		}
		interop.End();
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
			diagnostics.DumpTexture("02_output", eyes[0].output.texture->resource.get(), diagnostic.number);
		}
		if (diagnostic.options & NR::Diagnostics::BypassWriteback) {
			diagnostics.FinishCapture(diagnostic.number);
			return true;
		}
		for (uint32_t i = 0; i < eyeCount; ++i) {
			TransferColor(i, false);
		}
		if (capture) {
			diagnostics.DumpTexture("03_pre_composite", original->resource.get(), diagnostic.number);
			diagnostics.DumpTexture("NR_mask", reactive->resource.get(), diagnostic.number);
		}
		const D3D11_BOX box{ 0, 0, 0, width, height, 1 };
		for (uint32_t i = 0; i < eyeCount; ++i) {
			context->CopySubresourceRegion(color, 0, i * width, 0, 0, eyes[i].resolved->resource.get(), 0, &box);
			diagnostic.copied |= 1u << i;
		}
		if (capture)
			diagnostics.DumpTexture("04_post_composite", color, diagnostic.number);
		maskFrame = globals::state->frameCount;
		diagnostics.FinishCapture(diagnostic.number);
		captureDiagnostics = nullptr;
		return true;
	}
};

NeuralRendering::NeuralRendering() : impl(std::make_unique<Impl>()) {}
NeuralRendering::~NeuralRendering() = default;

void NeuralRendering::SetupResources() { retryRequested = recreate = resetHistory = true; }
void NeuralRendering::ResetHistory() { resetHistory = true; }
void NeuralRendering::ClearShaderCache() { retryRequested = clearShaders = resetHistory = true; }

void NeuralRendering::Reset(bool enabled)
{
	diagnostics.EndFrame(globals::state->frameCount, enabled, globals::state->worldRenderedThisFrame, globals::state->IsPausedOrMenuOpen(globals::game::ui));
	if (!enabled)
		retryRequested = true;
	if (!enabled || !globals::state->worldRenderedThisFrame)
		resetHistory = true;
}

void NeuralRendering::SetStatus(std::string message)
{
	std::scoped_lock lock(statusMutex);
	status = std::move(message);
}

void NeuralRendering::DrawSettings(bool& enabled, NR::Tuning& tuning)
{
	ImGui::PushID("NeuralRendering");
	if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
		retryRequested = resetHistory = true;
	ImGui::TextWrapped("One display-referred NR proxy pass at eye render resolution, composed back into scene-linear HDR before DLSS/FSR and frame-generation capture. Keep Reset NR every frame off for normal use. Requires an NR-capable NVIDIA GPU and the 310.8.x runtime.");
	int style = static_cast<int>(std::min(tuning.style, NR::Tuning::kMaxStyle));
	bool changed = ImGui::Combo("Style", &style, "Style 0\0Style 1\0Style 2\0");
	bool recreateTuning = changed;
	if (changed)
		tuning.style = static_cast<uint32_t>(style);
	changed |= ImGui::SliderFloat("Intensity", &tuning.intensity, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat("Local Tone Strength", &tuning.localToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat("Local Structure Strength", &tuning.localStructureStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat("Skin Structure Strength", &tuning.skinStructureStrength, NR::Tuning::kAutomaticSkinStructure, NR::Tuning::kMaxStrength,
		tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure ? "Auto" : "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	const bool autoMaskChanged = ImGui::Checkbox("Use Auto Mask", &tuning.useAutoMask);
	changed |= autoMaskChanged;
	recreateTuning |= autoMaskChanged;
	if (ImGui::Button("Restore NR Defaults")) {
		tuning = {};
		changed = recreateTuning = true;
	}
	if (changed)
		tuning.Sanitize();
	if (recreateTuning)
		recreate = resetHistory = true;
	ImGui::SameLine();
	if (ImGui::Button("Reset NR History"))
		resetHistory = true;
	if (enabled && ImGui::Button("Retry NR")) {
		retryRequested = resetHistory = true;
		SetStatus("Retry queued for the next rendered world frame");
	}
	if (globals::state->IsDeveloperMode())
		diagnostics.DrawSettings();
	std::scoped_lock lock(statusMutex);
	ImGui::TextWrapped("%s", enabled ? status.c_str() : "Disabled");
	ImGui::PopID();
}

void NeuralRendering::DrawDiagnosticsOverlay(bool enabled)
{
	std::string message;
	{
		std::scoped_lock lock(statusMutex);
		message = status;
	}
	diagnostics.DrawOverlay(enabled, message);
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
	if (!enabled) {
		diagnostic.outcome = Outcome::Disabled;
		retryRequested = resetHistory = true;
		return;
	}
	auto* state = globals::state;
	if (!state->worldRenderedThisFrame) {
		diagnostic.outcome = Outcome::NoWorld;
		resetHistory = true;
		return;
	}
	try {
		if (retryRequested.exchange(false) && impl->failed) {
			// Retire both APIs before releasing a failed runtime and its shared resources.
			impl->interop.Drain();
			impl = std::make_unique<Impl>();
			resetHistory = true;
		}
		auto& work = *impl;
		diagnostic.options = diagnostics.Options();
		if (diagnostic.options != work.lastDiagnosticOptions) {
			resetHistory = true;
			work.lastDiagnosticOptions = diagnostic.options;
		}
		if (work.failed) {
			diagnostic.outcome = Outcome::FailedLatch;
			return;
		}
		if (work.lastFrame == state->frameCount) {
			++diagnostic.duplicates;
			return;
		}
		if (!work.ready)
			work.Initialize();
		if (clearShaders.exchange(false)) {
			work.encode.Reset();
			work.prepareColor.Reset();
			work.compositeColor.Reset();
		}
		auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto* color = Util::AsReal(targets[RE::RENDER_TARGETS::kMAIN].texture);
		ID3D11ShaderResourceView* inputs[]{ Util::AsReal(targets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK].SRV),
			Util::AsReal(targets[globals::deferred->forwardRenderTargets[2]].SRV), Util::AsReal(targets[RE::RENDER_TARGETS::kMOTION_VECTOR].SRV), Util::AsReal(depth.depthSRV) };
		const auto count = globals::game::isVR ? 2u : 1u;
		const auto gw = static_cast<uint32_t>(renderSize.x) / count;
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
		if (!w || !h || !gw || !gh || desc.Width < w * count || desc.Height < h || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
			(desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
				desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM && desc.Format != DXGI_FORMAT_R11G11B10_FLOAT))
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
		// NR depth and motion must describe the same pixel; the DLSS permutation dilates only motion.
		auto* shader = work.encode.Get(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl",
			{ { "DEPTH_OUTPUT", "" } }, "cs_5_0", "main", "NeuralRendering::Encode CS");
		auto* prepare = work.prepareColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "Prepare", "NeuralRendering::PrepareColor CS");
		auto* composite = work.compositeColor.Get(L"Data/Shaders/Upscaling/NeuralRendering/ColorTransferCS.hlsl",
			{}, "cs_5_0", "Composite", "NeuralRendering::CompositeHDR CS");
		if (!shader || !prepare || !composite)
			throw std::runtime_error("NR encoder or color-transfer shader unavailable");
		work.debugOptions = diagnostic.options;
		work.conversionMode = diagnostics.ConversionMode();
		work.exposureMode = diagnostics.ExposureSetting();
		work.compositeMode = diagnostics.CompositionMode();
		work.visualMode = diagnostics.ViewMode();
		work.manualExposure = diagnostics.ManualExposure();
		work.differenceStrength = diagnostics.DifferenceStrength();
		work.splitPosition = diagnostics.SplitPosition();
		work.shadowProtect = diagnostics.ShadowProtect();
		work.highlightProtect = diagnostics.HighlightProtect();
		work.toneRadius = diagnostics.ToneRadius();
		uint32_t reset = resetHistory.exchange(false) ? NR::Diagnostics::Requested : 0;
		if (work.lastFrame == UINT32_MAX)
			reset |= NR::Diagnostics::FirstFrame;
		else if (work.lastFrame + 1 != state->frameCount)
			reset |= NR::Diagnostics::FrameGap;
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
		if (!work.Draw(color, inputs, shader, reset, boundedTuning, diagnostic, diagnostics))
			throw std::runtime_error(std::format("SDR-proxy Feature 18 creation/evaluation failed (NGX L/R: 0x{:08X}/0x{:08X})", diagnostic.result[0], diagnostic.result[1]));
		if (reset)
			SetStatus(std::format("Active: SDR proxy into scene-linear HDR, {} x {}, {} eye(s), before upscaling", w, h, count));
		diagnostic.outcome = (diagnostic.options & NR::Diagnostics::BypassWriteback) ? Outcome::Bypassed : Outcome::Applied;
		work.lastFrame = state->frameCount;
	} catch (const winrt::hresult_error& error) {
		diagnostic.outcome = Outcome::Error;
		impl->failed = true;
		SetStatus(std::format("NR paused: {}. Use Retry NR after correcting the error.", winrt::to_string(error.message())));
		logger::error("[NeuralRendering] D3D initialization/dispatch failed: 0x{:08X}", static_cast<uint32_t>(error.code().value));
	} catch (const std::exception& error) {
		diagnostic.outcome = Outcome::Error;
		impl->failed = true;
		SetStatus(std::format("NR paused: {}. Use Retry NR after correcting the error.", error.what()));
		logger::error("[NeuralRendering] {}", error.what());
	}
}

ID3D11ShaderResourceView* NeuralRendering::GetReactiveMask() const
{
	return !impl->failed && impl->maskFrame == globals::state->frameCount &&
	               globals::features::upscaling.settings.neuralRenderingEnabled && impl->reactive ?
	           impl->reactive->srv.get() :
	           nullptr;
}
