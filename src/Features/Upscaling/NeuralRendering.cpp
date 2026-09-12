#include "NeuralRendering.h"

#include "Deferred.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "NeuralRendering/D3D12Interop.h"
#include "NeuralRendering/Runtime.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/LazyShader.h"

struct NeuralRendering::Impl
{
	NR::D3D12Interop interop;
	NR::Runtime runtime;
	struct Eye
	{
		NR::SharedTexture color, depth, motion, output;
		NR::FrameParameters frame;
		DirectX::SimpleMath::Vector3 position{}, forward{};
	};
	std::array<Eye, 2> eyes;
	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> isolated;
	Util::LazyShader<ID3D11ComputeShader> encode;
	std::unique_ptr<ConstantBuffer> encodeBuffer;
	std::array<std::unique_ptr<Texture2D>, 2> encodeMasks;
	winrt::com_ptr<ID3D11Texture2D> source;
	uint32_t width = 0, height = 0, eyeCount = 0, lastFrame = UINT32_MAX;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool ready = false, failed = false;

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
		runtime.Initialize(interop.Device(), std::filesystem::absolute(Upscaling::streamline.pluginDir));
		ready = true;
	}

	void EnsureResources(ID3D11Texture2D* color, uint32_t w, uint32_t h, uint32_t count, DXGI_FORMAT colorFormat, bool force)
	{
		if (!force && source.get() == color && width == w && height == h && eyeCount == count && format == colorFormat)
			return;
		interop.Drain();
		runtime.ResetFeatures();
		eyes = {};
		lastFrame = UINT32_MAX;
		D3D11_TEXTURE2D_DESC maskDesc{};
		maskDesc.Width = w;
		maskDesc.Height = h;
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
		for (uint32_t i = 0; i < count; ++i) {
			auto& eye = eyes[i];
			const auto name = std::format("NeuralRendering::Eye{}", i);
			eye.color = interop.CreateTexture(w, h, colorFormat, name + " Color");
			eye.depth = interop.CreateTexture(w, h, DXGI_FORMAT_R32_FLOAT, name + " Depth");
			eye.motion = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16_FLOAT, name + " Motion");
			eye.output = interop.CreateTexture(w, h, colorFormat, name + " Output");
		}
		source.copy_from(color);
		width = w;
		height = h;
		eyeCount = count;
		format = colorFormat;
	}

	void UpdateFrame(uint32_t i, bool reset)
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
		eye.frame.reset = reset || (position - eye.position).LengthSquared() > kCameraCutDistance * kCameraCutDistance ||
		                  forward.Dot(eye.forward) < kCameraCutDirectionDot ||
		                  std::abs(projection._11 - eye.frame.viewToClip._11) > kProjectionCutThreshold ||
		                  std::abs(projection._22 - eye.frame.viewToClip._22) > kProjectionCutThreshold;
		eye.position = position;
		eye.forward = forward;
		eye.frame.worldToView = inverseView.Invert();
		eye.frame.viewToClip = projection;
		const auto jitter = globals::features::upscaling.jitter;
		eye.frame.jitterX = -jitter.x;
		eye.frame.jitterY = -jitter.y;
		eye.frame.frameTimeMs = *globals::game::deltaTime * 1000.0f;
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

	bool Draw(ID3D11Texture2D* color, ID3D11ShaderResourceView* const* inputs, ID3D11ComputeShader* shader, bool reset, const NR::Tuning& tuning)
	{
		CS_GPU_PASS("NeuralRendering::Evaluate");
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
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetShaderResources(0, 4, inputs);
		auto shared = globals::state->sharedDataCB->CB();
		context->CSSetConstantBuffers(5, 1, &shared);
		for (uint32_t i = 0; i < eyeCount; ++i) {
			auto& eye = eyes[i];
			UpdateFrame(i, reset);
			D3D11_BOX box{ i * width, 0, 0, (i + 1) * width, height, 1 };
			context->CopySubresourceRegion(eye.color.texture->resource.get(), 0, 0, 0, 0, color, 0, &box);
			Upscaling::UpscalingDataCB data{ { float(width), float(height) }, i * width, 0 };
			encodeBuffer->Update(data);
			auto buffer = encodeBuffer->CB();
			context->CSSetConstantBuffers(0, 1, &buffer);
			// The shared encoder writes both masks even though NR only consumes motion and depth.
			ID3D11UnorderedAccessView* outputs[]{ encodeMasks[0]->uav.get(), encodeMasks[1]->uav.get(),
				eye.motion.texture->uav.get(), eye.depth.texture->uav.get() };
			context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		}
		context->ClearState();
		auto* commands = interop.Begin();
		bool success = true;
		for (uint32_t i = 0; i < eyeCount && success; ++i) {
			auto& eye = eyes[i];
			Transition(commands, eye, true);
			success = runtime.Evaluate(commands, i, eye.color.resource.get(), eye.depth.resource.get(),
				eye.motion.resource.get(), eye.output.resource.get(), width, height, eye.frame, tuning);
			Transition(commands, eye, false);
		}
		interop.End();
		if (!success)
			return false;
		const D3D11_BOX box{ 0, 0, 0, width, height, 1 };
		for (uint32_t i = 0; i < eyeCount; ++i)
			context->CopySubresourceRegion(color, 0, i * width, 0, 0, eyes[i].output.texture->resource.get(), 0, &box);
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
	if (!enabled)
		retryRequested = true;
	if (!enabled || !globals::state->worldRenderedThisFrame || globals::state->IsPausedOrMenuOpen(globals::game::ui))
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
	ImGui::TextWrapped("One pass at the current eye render resolution, before upscaling. Requires an NR-capable NVIDIA GPU and the 310.8.x runtime.");
	bool changed = ImGui::SliderFloat("Intensity", &tuning.intensity, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat("Local Tone", &tuning.localToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat("Local Structure", &tuning.localStructureStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	changed |= ImGui::SliderFloat("Skin Structure", &tuning.skinStructureStrength, NR::Tuning::kAutomaticSkinStructure, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::TextUnformatted("Skin Structure: -1 uses the runtime default.");
	if (ImGui::Button("Restore NR Defaults")) {
		tuning = {};
		changed = true;
	}
	if (changed) {
		tuning.Sanitize();
		resetHistory = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset NR History"))
		resetHistory = true;
	if (enabled && ImGui::Button("Retry NR")) {
		retryRequested = resetHistory = true;
		SetStatus("Retry queued for the next rendered world frame");
	}
	std::scoped_lock lock(statusMutex);
	ImGui::TextWrapped("%s", enabled ? status.c_str() : "Disabled");
	ImGui::PopID();
}

void NeuralRendering::DrawBeforeUpscaling(bool enabled, const NR::Tuning& tuning)
{
	if (!enabled) {
		retryRequested = resetHistory = true;
		return;
	}
	auto* state = globals::state;
	if (!state->worldRenderedThisFrame || state->IsPausedOrMenuOpen(globals::game::ui)) {
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
		if (work.failed || work.lastFrame == state->frameCount)
			return;
		if (!work.ready)
			work.Initialize();
		if (clearShaders.exchange(false))
			work.encode.Reset();
		auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto* color = targets[RE::RENDER_TARGETS::kMAIN].texture;
		ID3D11ShaderResourceView* inputs[]{ targets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK].SRV,
			targets[globals::deferred->forwardRenderTargets[2]].SRV, targets[RE::RENDER_TARGETS::kMOTION_VECTOR].SRV, depth.depthSRV };
		const auto count = globals::game::isVR ? 2u : 1u;
		const auto renderSize = Util::ConvertToDynamic(state->screenSize);
		const auto& submit = globals::features::upscaling.vrSubmit;
		const auto w = submit.IsHookActive() ? submit.GetRenderEyeWidth() : static_cast<uint32_t>(renderSize.x) / count;
		const auto h = submit.IsHookActive() ? submit.GetRenderEyeHeight() : static_cast<uint32_t>(renderSize.y);
		D3D11_TEXTURE2D_DESC desc{};
		if (color)
			color->GetDesc(&desc);
		if (!w || !h || desc.Width < w * count || desc.Height < h || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
			(desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM))
			throw std::runtime_error("Unsupported NR color extent or format");
		for (auto* input : inputs) {
			D3D11_TEXTURE2D_DESC guide{};
			if (!input || !Util::GetTexture2DDesc(input, guide) || guide.Width < w * count || guide.Height < h ||
				guide.ArraySize != 1 || guide.SampleDesc.Count != 1)
				throw std::runtime_error("Missing or incompatible NR guide texture");
		}
		work.EnsureResources(color, w, h, count, desc.Format, recreate.exchange(false));
		auto* shader = work.encode.Get(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl",
			{ { "DLSS", "" }, { "DEPTH_OUTPUT", "" } }, "cs_5_0", "main", "NeuralRendering::Encode CS");
		if (!shader)
			throw std::runtime_error("Upscaling encoder unavailable for NR");
		const bool reset = resetHistory.exchange(false) || work.lastFrame == UINT32_MAX || work.lastFrame + 1 != state->frameCount;
		auto boundedTuning = tuning;
		boundedTuning.Sanitize();
		if (!work.Draw(color, inputs, shader, reset, boundedTuning))
			throw std::runtime_error("Feature 18 failed; see the NGX result in the log");
		if (reset)
			SetStatus(std::format("Active: {} x {}, {} eye(s)", w, h, count));
		work.lastFrame = state->frameCount;
	} catch (const winrt::hresult_error& error) {
		impl->failed = true;
		SetStatus(std::format("NR paused: {}. Use Retry NR after correcting the error.", winrt::to_string(error.message())));
		logger::error("[NeuralRendering] D3D initialization/dispatch failed: 0x{:08X}", static_cast<uint32_t>(error.code().value));
	} catch (const std::exception& error) {
		impl->failed = true;
		SetStatus(std::format("NR paused: {}. Use Retry NR after correcting the error.", error.what()));
		logger::error("[NeuralRendering] {}", error.what());
	}
}
