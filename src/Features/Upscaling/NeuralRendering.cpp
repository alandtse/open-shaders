#include "NeuralRendering.h"

#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "NeuralRendering/ActorRegion.h"
#include "NeuralRendering/D3D12Interop.h"
#include "NeuralRendering/Lifecycle.h"
#include "NeuralRendering/Runtime.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/LazyShader.h"
#include "Utils/RegionOverlay.h"
#include "Utils/Subrect.h"
#include "Utils/UI.h"

#include <chrono>
#include <span>

#define I18N_KEY_PREFIX "feature.upscaling.neural_rendering."

namespace
{
	/** @brief Active-status line; malformed braces in a translation fall back to the English text. */
	std::string FormatActiveStatus(const std::string& runtime)
	{
		try {
			return std::vformat(T(TKEY("status_active"), "Active (runtime {})"), std::make_format_args(runtime));
		} catch (const std::format_error&) {
			return std::format("Active (runtime {})", runtime);
		}
	}

	/** @brief A trackable actor, held by handle so the crop never dereferences a stale pointer. */
	struct RegionCandidate
	{
		RE::ActorHandle handle;
		float score;
	};

	/**
	 * @brief Projects an actor's world-space bound points into one eye's screen bounds.
	 *        The view-projection matrix takes camera-relative input, and the origin is per eye.
	 */
	Util::Region::ProjectionResult ProjectActorEyeBounds(const Util::BoundPoints& a_worldPoints, uint32_t a_eye, Util::Region::ScreenBounds& a_out)
	{
		const auto origin = Util::GetEyePosition(static_cast<int>(a_eye));
		const float3 eyeOrigin{ origin.x, origin.y, origin.z };
		Util::BoundPoints relative;
		for (const auto& point : a_worldPoints.View())
			relative.Add(point - eyeOrigin);
		return Util::Region::ProjectBounds(Util::GetCameraData(static_cast<int>(a_eye)).viewProjMat, relative.View(), a_out);
	}

	/**
	 * @brief Prominence of an actor this frame, as the best of its on-screen bounds over the eyes that see it.
	 *        Projection only: the line-of-sight rays stay in ProjectActorRegion, which runs on the
	 *        best-scoring candidates alone.
	 */
	float ActorProminenceScore(RE::Actor* a_actor, uint32_t a_eyes, bool a_incumbent)
	{
		const auto worldPoints = Util::GetActorBoundPoints(*a_actor, false);
		float best = 0.0f;
		for (uint32_t eye = 0; eye < a_eyes; ++eye) {
			Util::Region::ScreenBounds bounds;
			const auto projection = ProjectActorEyeBounds(worldPoints, eye, bounds);
			if (projection == Util::Region::ProjectionResult::kOffscreen)
				continue;
			// An actor crossing the eye plane fills the view, so it outranks any bounded candidate.
			if (projection == Util::Region::ProjectionResult::kBehindEye) {
				best = std::max(best, a_incumbent ? NR::ActorRegion::kIncumbentScoreBonus : 1.0f);
				continue;
			}
			if (Util::Region::AreaFraction(bounds) < NR::ActorRegion::kMinVisibleAreaFraction)
				continue;
			best = std::max(best, NR::ActorRegion::ActorScore(bounds, a_incumbent));
		}
		return best;
	}

	/**
	 * @brief Projects an actor's bound into per-eye crops, leaving the full frame where an eye
	 *        cannot be cropped; false when no eye sees it, so it cannot be tracked.
	 * @param a_actorBox Receives the same bound with no padding and no grid alignment: empty per eye
	 *        the projection missed, and the whole frame where the bound is behind the eye.
	 */
	bool ProjectActorRegion(RE::Actor* a_actor, Util::Region::StereoRegion& a_region, Util::Region::StereoRegion& a_actorBox,
		uint32_t a_eyeWidth, uint32_t a_eyeHeight, uint32_t a_eyes, uint32_t a_fit)
	{
		const Util::Subrect::PixelRegion frame{ 0, 0, a_eyeWidth, a_eyeHeight };
		a_region.eye.fill(frame);
		a_actorBox.eye.fill(Util::Region::kEmptyRegion);
		const auto& padding = a_fit == NR::Tuning::kRegionFitTight ? NR::ActorRegion::kTightPadding : NR::ActorRegion::kPadding;
		const auto worldPoints = Util::GetActorBoundPoints(*a_actor, true, NR::ActorRegion::kJointMargin);
		bool tracked = false;
		for (uint32_t eye = 0; eye < a_eyes; ++eye) {
			Util::Region::ScreenBounds bounds;
			const auto projection = ProjectActorEyeBounds(worldPoints, eye, bounds);
			if (projection == Util::Region::ProjectionResult::kOffscreen ||
				!Util::IsActorVisibleFromEye(*a_actor, Util::GetEyePosition(static_cast<int>(eye))))
				continue;
			tracked = true;
			if (projection == Util::Region::ProjectionResult::kBehindEye) {
				a_actorBox.eye[eye] = frame;
				continue;
			}
			const auto crop = Util::Region::PixelRegionFromBounds(bounds, a_eyeWidth, a_eyeHeight, padding);
			if (crop.w && crop.h)
				a_region.eye[eye] = crop;
			a_actorBox.eye[eye] = Util::Region::PixelRegionFromBounds(bounds, a_eyeWidth, a_eyeHeight,
				NR::ActorRegion::kTightPadding, Util::Region::kNoPixelAlignment);
		}
		a_actorBox.active = tracked;
		return tracked;
	}

	/** @brief Pass-to-pass timing ratio above which a calibration result is flagged as unsteady. */
	constexpr float kCalibrationUnstableRatio = 1.5f;

	/** @brief Width the debug region overlay draws the crop outline at, in NR render-resolution pixels. */
	constexpr float kRegionOutlineThicknessPixels = 3.0f;

	/** @brief Colour the preview draws the tracked actor's projected box in; the shader outline uses the same yellow. */
	constexpr ImU32 kActorBoxPreviewColor = IM_COL32(255, 255, 0, 255);
}

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
		uint32_t regionBaseX = 0, regionBaseY = 0, regionWidth = 0, regionHeight = 0;
		uint32_t regionOverlayEnabled = 0;
		float regionOutlineThickness = kRegionOutlineThicknessPixels;
		uint32_t regionActorBaseX = 0, regionActorBaseY = 0, regionActorWidth = 0, regionActorHeight = 0;
		float2 pad{};
	};
	static_assert(offsetof(ColorTransferData, dynamicRangeProtect) == 64);
	static_assert(offsetof(ColorTransferData, toneLowStrength) == 80);
	static_assert(offsetof(ColorTransferData, toneRadius) == 84);
	static_assert(offsetof(ColorTransferData, toneHighStrength) == 88);
	static_assert(offsetof(ColorTransferData, hasToneData) == 92);
	static_assert(offsetof(ColorTransferData, regionBaseX) == 96);
	static_assert(offsetof(ColorTransferData, regionOverlayEnabled) == 112);
	static_assert(offsetof(ColorTransferData, regionOutlineThickness) == 116);
	static_assert(offsetof(ColorTransferData, regionActorBaseX) == 120);
	static_assert(offsetof(ColorTransferData, regionActorHeight) == 132);
	static_assert(offsetof(ColorTransferData, pad) == 136);
	static_assert(sizeof(ColorTransferData) == 144);
	std::unique_ptr<ConstantBuffer> colorBuffer;
	std::unique_ptr<Texture2D> original;
	std::unique_ptr<ConstantBuffer> encodeBuffer;
	std::array<std::unique_ptr<Texture2D>, 2> encodeMasks;
	uint32_t width = 0, height = 0, guideWidth = 0, guideHeight = 0, eyeCount = 0, lastFrame = UINT32_MAX;
	/** @brief Crop of the last frame NR evaluated; inactive means it covered the whole frame. */
	Util::Region::StereoRegion lastRegion;
	/** @brief Crop this frame's evaluation uses, published by the main thread and read by TransferColor. */
	Util::Region::StereoRegion region;
	/** @brief The tracked actor's unpadded projected box, published alongside the crop and drawn by the overlay. */
	Util::Region::StereoRegion actorBox;
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
	bool useResolutionMotionScale = true;
	/** @brief Draws the evaluated crop outline into the composite; read from the tuning each NR frame. */
	bool regionOverlay = false;
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
		lastRegion = {};
	}

	/** @brief True while pass resources for a render size exist and can be released. */
	bool HasPassResources() const { return eyeCount != 0; }

	/** @brief Publishes this frame's crop for one eye; every kernel that reads a neural sample needs it. */
	void SetRegion(ColorTransferData& a_data, uint32_t a_eye) const
	{
		if (region.active) {
			const auto& crop = region.eye[a_eye];
			a_data.regionBaseX = crop.x;
			a_data.regionBaseY = crop.y;
			a_data.regionWidth = crop.w;
			a_data.regionHeight = crop.h;
		}
		if (actorBox.active) {
			const auto& box = actorBox.eye[a_eye];
			a_data.regionActorBaseX = box.x;
			a_data.regionActorBaseY = box.y;
			a_data.regionActorWidth = box.w;
			a_data.regionActorHeight = box.h;
		}
	}

	void EnsureResources(uint32_t w, uint32_t h, uint32_t gw, uint32_t gh, uint32_t count, DXGI_FORMAT colorFormat, bool force)
	{
		if (!force && width == w && height == h && guideWidth == gw && guideHeight == gh && eyeCount == count && format == colorFormat)
			return;
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
		SetRegion(data, i);
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
		// Never on the Prepare dispatch: that writes NGX's input proxy, which the overlay would corrupt.
		data.regionOverlayEnabled = (!prepare && regionOverlay) ? 1u : 0u;
		data.regionOutlineThickness = kRegionOutlineThicknessPixels;
		SetRegion(data, i);
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
				const auto started = std::chrono::steady_clock::now();
				interop.Drain();
				diagnostic.resetDrainMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
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
					const auto crop = region.active ? Util::Region::ClampToFrame(region.eye[i], width, height) : Util::Region::kEmptyRegion;
					const auto eyeRegion = (crop.w && crop.h) ? NR::ToGuideRegion(crop) : NR::GuideRegion{ 0, 0, width, height };
					NR::GuideParameters guides;
					guides.depth = eyeRegion;
					guides.motion = eyeRegion;
					guides.colorOutput = eyeRegion;
					// MotionBlur produces normalized eye-UV displacement; NR consumes input-pixel displacement.
					guides.motionScaleX = useResolutionMotionScale ? static_cast<float>(width) : 1.0f;
					guides.motionScaleY = useResolutionMotionScale ? static_cast<float>(height) : 1.0f;
					success = runtime.Evaluate(commands, i, eye.color->resource.get(), eye.depth->resource.get(),
						eye.motion->resource.get(), eye.output->resource.get(), width, height, guides, eye.frame, tuning);
				}
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
		// Capture stays open for Main_PostProcessing's pre-SR/post-SR stages; CaptureAfterUpscaling finishes it.
		captureDiagnostics = nullptr;
		return true;
	}
};

namespace
{
	/** @brief Actor-tracking call site, shared with GrassCollision; SKSE chains both hooks. */
	struct MainUpdate_UpdateRegionOfInterest
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void MainUpdate_UpdateRegionOfInterest::thunk()
	{
		func();
		globals::features::upscaling.neuralRendering.UpdateRegionOfInterest();
	}
}

NeuralRendering::NeuralRendering() :
	impl(std::make_unique<Impl>()) {}
NeuralRendering::~NeuralRendering() = default;

void NeuralRendering::InstallHooks()
{
	const std::uintptr_t aeOffset = REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0xC38 : 0xC26;
	stl::write_thunk_call<MainUpdate_UpdateRegionOfInterest>(
		REL::RelocationID(35565, 36564).address() + REL::Relocate<std::uintptr_t>(0x748, aeOffset, 0x7EE));
	logger::debug("[NeuralRendering] Installed actor-tracking hook");
}

void NeuralRendering::UpdateCalibration()
{
	auto& upscaling = globals::features::upscaling;
	const auto resources = GetStatus();
	if (upscaling.IsFrameGenerationActive()) {
		calibration.Fail(NR::CropCalibration::Failure::kFrameGeneration);
	} else if (publishedState.load(std::memory_order_relaxed) != Status::State::kActive || !resources.eyes || !resources.width || !resources.height) {
		calibration.Fail(NR::CropCalibration::Failure::kNotActive);
	} else {
		float gpuMs = 0.0f;
		if (globals::profiler) {
			globals::profiler->RequestCapture();
			for (const auto& timer : globals::profiler->GetResults()) {
				if (timer.valid && timer.activeGpu && timer.name == "Upscaling::NREvaluate")
					gpuMs = timer.gpuTimeMs;
			}
		}
		calibration.AddFrame(gpuMs);
	}
	Util::Region::StereoRegion forced;
	if (calibration.Running() && calibration.CurrentFraction() < 1.0f) {
		const auto bounds = NR::CenteredBounds(calibration.CurrentFraction());
		for (uint32_t eye = 0; eye < resources.eyes; ++eye)
			forced.eye[eye] = Util::Region::PixelRegionFromBounds(bounds, resources.width, resources.height, NR::ActorRegion::kTightPadding);
		forced.active = true;
	}
	if (calibration.GetResult().state == NR::CropCalibration::State::kDone)
		calibratedKneeFraction = calibration.GetResult().kneeFraction;
	std::scoped_lock lock(regionMutex);
	regionStabilizer.Reset();
	region = forced;
	actorBox = {};
	calibrationResult = calibration.GetResult();
}

NR::CropCalibration::Result NeuralRendering::GetCalibration() const
{
	std::scoped_lock lock(regionMutex);
	return calibrationResult;
}

void NeuralRendering::UpdateRegionOfInterest()
{
	if (calibrationRequested.exchange(false, std::memory_order_relaxed)) {
		calibration.Start();
		std::scoped_lock lock(regionMutex);
		calibrationResult = calibration.GetResult();
	}
	if (calibration.Running()) {
		UpdateCalibration();
		return;
	}
	Util::Region::StereoRegion next, nextActorBox;
	uint32_t eyeWidth = 0, eyeHeight = 0;
	RE::ActorHandle winner;
	if (regionEnabled.load(std::memory_order_relaxed) && publishedState.load(std::memory_order_relaxed) == Status::State::kActive) {
		const auto resources = GetStatus();
		const auto eyes = resources.eyes;
		eyeWidth = eyes ? resources.width : 0u;
		eyeHeight = resources.height;
		if (eyeWidth && eyeHeight) {
			const auto fit = regionFit.load(std::memory_order_relaxed);
			const bool group = regionGroup.load(std::memory_order_relaxed);
			const auto camera = Util::GetEyePosition(0);
			constexpr float maxSqDistance = NR::ActorRegion::kMaxActorDistance * NR::ActorRegion::kMaxActorDistance;
			// In first person and VR the player sits at the camera, so tracking it would crop to the near plane.
			const auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const bool playerVisible = !globals::game::isVR && playerCamera && playerCamera->IsInThirdPerson();
			std::vector<RegionCandidate> candidates;
			Util::ForEachLoadedActor([&](RE::Actor* a_actor) {
				if (!a_actor || !a_actor->Is3DLoaded() || (a_actor == globals::game::player && !playerVisible))
					return;
				if (camera.GetSquaredDistance(a_actor->GetPosition()) > maxSqDistance)
					return;
				const auto handle = a_actor->GetHandle();
				const bool incumbent = static_cast<bool>(trackedActor) && handle == trackedActor;
				const float score = ActorProminenceScore(a_actor, eyes, incumbent);
				if (score > 0.0f)
					candidates.push_back({ handle, score });
			});
			std::sort(candidates.begin(), candidates.end(), [](const RegionCandidate& a_left, const RegionCandidate& a_right) {
				return a_left.score > a_right.score;
			});
			uint32_t members = 0, tested = 0;
			for (const auto& candidate : candidates) {
				auto actor = candidate.handle.get();
				if (!actor)
					continue;
				if (!winner) {
					if (!ProjectActorRegion(actor.get(), next, nextActorBox, eyeWidth, eyeHeight, eyes, fit))
						continue;
					next.active = true;
					winner = candidate.handle;
					if (!group)
						break;
					members = 1;
					continue;
				}
				if (members >= NR::ActorRegion::kMaxGroupActors || tested++ >= NR::ActorRegion::kMaxGroupCandidatesTested)
					break;
				Util::Region::StereoRegion memberRegion, memberBox;
				if (!ProjectActorRegion(actor.get(), memberRegion, memberBox, eyeWidth, eyeHeight, eyes, fit))
					continue;
				for (uint32_t eye = 0; eye < eyes; ++eye) {
					if (!memberBox.eye[eye].w || !memberBox.eye[eye].h)
						memberRegion.eye[eye] = Util::Region::kEmptyRegion;
				}
				if (!Util::Region::TryMergeRegions(next, memberRegion, eyeWidth, eyeHeight, eyes, NR::ActorRegion::GroupAreaCap(calibratedKneeFraction)))
					continue;
				for (uint32_t eye = 0; eye < eyes; ++eye)
					nextActorBox.eye[eye] = Util::Region::UnionNonEmpty(nextActorBox.eye[eye], memberBox.eye[eye]);
				++members;
			}
		}
	}
	trackedActor = winner;
	std::scoped_lock lock(regionMutex);
	if (eyeWidth && eyeHeight) {
		region = regionStabilizer.Update(next, eyeWidth, eyeHeight);
		actorBox = nextActorBox;
	} else {
		regionStabilizer.Reset();
		region = {};
		actorBox = {};
	}
}

Util::Region::StereoRegion NeuralRendering::GetRegionOfInterest() const
{
	std::scoped_lock lock(regionMutex);
	return region;
}

Util::Region::StereoRegion NeuralRendering::GetActorBox() const
{
	std::scoped_lock lock(regionMutex);
	return actorBox;
}

void NeuralRendering::SetupResources() { retryRequested = recreate = resetHistory = true; }
void NeuralRendering::ResetHistory() { resetHistory = true; }
void NeuralRendering::ClearShaderCache() { retryRequested = clearShaders = resetHistory = true; }

void NeuralRendering::Reset(bool enabled, bool regionOfInterest, uint32_t cropFit, bool cropGroup)
{
	regionEnabled.store(enabled && regionOfInterest, std::memory_order_relaxed);
	regionFit.store(cropFit, std::memory_order_relaxed);
	regionGroup.store(cropGroup, std::memory_order_relaxed);
	diagnostics.SetDeveloperMode(globals::state->IsDeveloperMode());
	if (enabled)
		diagnostics.EndFrame(globals::state->frameCount, globals::state->worldRenderedThisFrame, globals::state->IsPausedOrMenuOpen(globals::game::ui));
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
	std::scoped_lock lock(statusMutex);
	status = std::move(next);
	publishedState.store(state, std::memory_order_relaxed);
}

void NeuralRendering::PublishFailure(const std::string& detail)
{
	PublishStatus(Status::State::kFailed, std::format("{} {}", T(TKEY("status_failed"), "Neural Rendering stopped:"), detail));
}

void NeuralRendering::PublishResources()
{
	std::scoped_lock lock(statusMutex);
	status.width = impl->width;
	status.height = impl->height;
	status.eyes = impl->eyeCount;
}

NeuralRendering::Status NeuralRendering::GetStatus() const
{
	std::scoped_lock lock(statusMutex);
	auto snapshot = status;
	if (snapshot.text.empty())
		snapshot.text = T(TKEY("status_off"), "Off");
	snapshot.lastAppliedFrame = appliedFrame.load(std::memory_order_relaxed);
	snapshot.appliedFrames = appliedFrames.load(std::memory_order_relaxed);
	snapshot.ngxResult = { lastNgxResult[0].load(std::memory_order_relaxed), lastNgxResult[1].load(std::memory_order_relaxed) };
	return snapshot;
}

void NeuralRendering::DrawSettings(bool& enabled, NR::Tuning& tuning)
{
	ImGui::PushID("NeuralRendering");
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable Neural Rendering"), &enabled))
		retryRequested = resetHistory = true;
	ImGui::TextWrapped("%s", T(TKEY("description"),
								 "One display-referred NR proxy pass at eye render resolution, composed back into scene-linear HDR before DLSS/FSR and frame-generation capture. Requires an NR-capable NVIDIA GPU and one of the validated 310.8 runtime builds listed in docs/development/neural-rendering.md."));
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
	if (ImGui::Checkbox(T(TKEY("region_of_interest"), "Limit to Tracked Actor"), &tuning.regionOfInterest)) {
		changed = true;
		resetHistory = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("region_of_interest_tooltip"),
			"Restricts Neural Rendering to a crop around the most prominent visible character, the one covering the most of the view with the centre favoured, and leaves the rest of the frame at pre-NR quality. Costs less GPU time when a character is on screen."));
	if (ImGui::Checkbox(T(TKEY("region_overlay"), "Show Region Overlay"), &tuning.regionOverlay))
		changed = true;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("region_overlay_tooltip"),
			"Draws the evaluated crop: a green outline in the game frame and the same rectangle over the preview below. Only meaningful with Limit to Tracked Actor on, and it draws nothing while no character is tracked, since the whole frame is evaluated then."));
	if (globals::state->IsDeveloperMode()) {
		if (ImGui::Checkbox(T(TKEY("crop_group"), "Track Multiple Characters"), &tuning.regionGroup)) {
			changed = true;
			resetHistory = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("crop_group_tooltip"),
				"Grows the crop to also cover the next most prominent characters while it stays under half the view, so a group is evaluated together. Off tracks one character."));
		int fit = static_cast<int>(std::min(tuning.regionFit, NR::Tuning::kMaxRegionFit));
		const std::array<const char*, NR::Tuning::kMaxRegionFit + 1> fitLabels{
			T(TKEY("crop_fit_padded"), "Padded"),
			T(TKEY("crop_fit_tight"), "Tight"),
		};
		if (ImGui::Combo(T(TKEY("crop_fit"), "Crop Fit"), &fit, fitLabels.data(), static_cast<int>(fitLabels.size()))) {
			tuning.regionFit = static_cast<uint32_t>(fit);
			changed = true;
			resetHistory = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("crop_fit_tooltip"),
				"How much margin the crop keeps around the tracked character. Padded keeps the normal margin; Tight evaluates the character's own outline with no margin, for checking what the crop covers."));
		if (ImGui::Button(T(TKEY("crop_calibrate"), "Calibrate Crop Cost")))
			RequestCalibration();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("crop_calibrate_tooltip"),
				"Measures how Neural Rendering's GPU time falls as the crop shrinks, over about half a minute, and finds the largest crop that is still about as cheap as the smallest. Turn frame generation off first; it skews the timing."));
		const auto calibrationState = GetCalibration();
		if (calibrationState.state == NR::CropCalibration::State::kRunning) {
			ImGui::TextUnformatted(T(TKEY("crop_calibrate_running"), "Calibrating..."));
		} else if (calibrationState.state == NR::CropCalibration::State::kFailed) {
			ImGui::TextUnformatted(T(TKEY("crop_calibrate_failed"), "Calibration failed: frame generation on, Neural Rendering not running, or no timing data."));
		} else if (calibrationState.state == NR::CropCalibration::State::kDone) {
			for (size_t step = 0; step < NR::CropCalibration::kSteps; ++step)
				ImGui::Text("%3.0f%%: %.2f ms", NR::CropCalibration::kFractions[step] * 100.0f, calibrationState.stepMs[step]);
			ImGui::Text(T(TKEY("crop_calibrate_knee"), "Largest crop that is still cheap: %.0f%%"), calibrationState.kneeFraction * 100.0f);
			if (calibrationState.stabilityRatio > kCalibrationUnstableRatio)
				ImGui::TextUnformatted(T(TKEY("crop_calibrate_unstable"), "Timing was unsteady between passes; run it again."));
		}
	}
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
	if (tuning.regionOverlay)
		DrawRegionPreview();
	ImGui::PopID();
}

void NeuralRendering::DrawRegionPreview()
{
	ImGui::Separator();
	// Panels draw on the rendering thread after the NR pass, so impl's preview texture needs no lock.
	const auto tracked = GetRegionOfInterest();
	const auto trackedBox = GetActorBox();
	const uint32_t sourceWidth = impl->width * impl->eyeCount;
	const uint32_t sourceHeight = impl->height;
	auto* preview = impl->original ? impl->original->srv.get() : nullptr;
	if (!preview || !sourceWidth || !sourceHeight) {
		ImGui::TextDisabled("%s", T(TKEY("region_overlay_unavailable"), "Crop preview appears once Neural Rendering runs a frame."));
		return;
	}
	const float maxWidth = std::min(400.0f, ImGui::GetContentRegionAvail().x);
	const float aspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
	const ImVec2 imageSize(maxWidth, maxWidth / aspect);
	const ImVec2 imageMin = ImGui::GetCursorScreenPos();
	Util::Subrect::ImageOpaque(preview, imageSize);
	std::array<Util::RegionOverlay::Region, 4> rects{};
	size_t count = 0;
	const uint32_t eyes = std::min<uint32_t>(impl->eyeCount, 2);
	if (tracked.active) {
		for (uint32_t eye = 0; eye < eyes; ++eye) {
			const auto& crop = tracked.eye[eye];
			rects[count].rect = Util::Subrect::PixelRegion{ crop.x + eye * impl->width, crop.y, crop.w, crop.h };
			rects[count].label = eyes > 1 ? (eye == 0 ? "L" : "R") : nullptr;
			++count;
		}
	}
	if (trackedBox.active) {
		for (uint32_t eye = 0; eye < eyes; ++eye) {
			const auto& box = trackedBox.eye[eye];
			rects[count].rect = Util::Subrect::PixelRegion{ box.x + eye * impl->width, box.y, box.w, box.h };
			rects[count].color = kActorBoxPreviewColor;
			rects[count].label = "actor";
			++count;
		}
	}
	Util::RegionOverlay::Draw(imageMin, imageSize, sourceWidth, sourceHeight, std::span(rects.data(), count));
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
		if (!impl->failed && publishedState.load(std::memory_order_relaxed) != Status::State::kOff) {
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
		// A rebuilt runtime starts uninitialized, so both actions end in Initialize().
		if (action == NR::FrameAction::InitializeThenRun || action == NR::FrameAction::RebuildThenRun) {
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
		work.EnsureResources(w, h, gw, gh, count, desc.Format, forceRecreate);
		if (diagnostic.recreated)
			PublishResources();
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
		work.region = GetRegionOfInterest();
		work.actorBox = GetActorBox();
		if (Util::Region::ShouldResetForRegion(NR::ActorRegion::kResetPolicy, work.region, work.lastRegion, NR::ActorRegion::kHistoryTolerancePixels))
			reset |= NR::Diagnostics::RegionChanged;
		work.lastRegion = work.region;
		if (diagnostic.options & NR::Diagnostics::DisableTone)
			boundedTuning.localToneStrength = 0.0f;
		if (diagnostic.options & NR::Diagnostics::DisableStructure)
			boundedTuning.localStructureStrength = 0.0f;
		if (diagnostic.options & NR::Diagnostics::DisableSkin)
			boundedTuning.skinStructureStrength = NR::Tuning::kAutomaticSkinStructure;
		work.toneLowStrength = boundedTuning.localToneStrength;
		work.toneHighStrength = boundedTuning.localStructureStrength;
		work.regionOverlay = boundedTuning.regionOverlay;
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
			throw std::runtime_error(std::format("the NVIDIA runtime failed to process a frame. Update the GPU driver, then press Retry (NGX L/R 0x{:08X}/0x{:08X})",
				diagnostic.result[0], diagnostic.result[1]));
		appliedFrame.store(state->frameCount, std::memory_order_relaxed);
		appliedFrames.fetch_add(1, std::memory_order_relaxed);
		const uint32_t frameResets = diagnostic.reset[0] | diagnostic.reset[1];
		diagnostics.RecordFrame(frameResets, diagnostic.resetDrainMs);
		lastNgxResult[0].store(diagnostic.result[0], std::memory_order_relaxed);
		lastNgxResult[1].store(diagnostic.result[1], std::memory_order_relaxed);
		if (publishedState.load(std::memory_order_relaxed) != Status::State::kActive) {
			const auto runtime = work.runtime.Version();
			const auto luid = work.interop.AdapterLuid();
			PublishStatus(Status::State::kActive, FormatActiveStatus(runtime));
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
