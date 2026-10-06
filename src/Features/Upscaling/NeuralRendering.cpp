#include "NeuralRendering.h"

#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "NeuralRendering/ActorRegion.h"
#include "NeuralRendering/D3D12Interop.h"
#include "NeuralRendering/FoveaClip.h"
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

	/** @brief Absolute plugin directory the runtime loads from; the load and the panel's verdict must resolve it alike. */
	std::filesystem::path RuntimeDirectory()
	{
		return Util::PathHelpers::SafeAbsolute(Upscaling::streamline.pluginDir);
	}

	/**
	 * @brief How to get a runtime the pass will load, for a verdict that found none it can use.
	 *        A missing file needs supplying; a rejected build needs replacing, never trusting it.
	 */
	const char* RuntimeFixHint(NR::RuntimeAvailability::State state)
	{
		if (state == NR::RuntimeAvailability::State::kMissing)
			return T(TKEY("runtime_missing_fix"), "Install nvngx_dlssnr.dll under Data\\Shaders\\Upscaling\\Streamline\\.");
		return T(TKEY("runtime_build_fix"), "Replace it with a validated 310.8 build of that file.");
	}

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
			if (projection == Util::Region::ProjectionResult::kBehindEye) {
				best = std::max(best, NR::ActorRegion::kEyePlaneCrossingScore * (a_incumbent ? NR::ActorRegion::kIncumbentScoreBonus : 1.0f));
				continue;
			}
			if (Util::Region::AreaFraction(bounds) < NR::ActorRegion::kMinVisibleAreaFraction)
				continue;
			best = std::max(best, NR::ActorRegion::ActorScore(bounds, a_incumbent));
		}
		return best;
	}

	/**
	 * @brief Projects an actor's bound into per-eye crops; false when no eye has a clear view of it,
	 *        so it cannot be tracked. An eye whose view is blocked keeps its own crop, and an eye the
	 *        bound misses takes the other eye's crop, so both eyes enhance the same scene area.
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
		std::array<bool, 2> projected{};
		for (uint32_t eye = 0; eye < a_eyes; ++eye) {
			Util::Region::ScreenBounds bounds;
			const auto projection = ProjectActorEyeBounds(worldPoints, eye, bounds);
			if (projection == Util::Region::ProjectionResult::kOffscreen)
				continue;
			projected[eye] = true;
			tracked |= Util::IsActorVisibleFromEye(*a_actor, Util::GetEyePosition(static_cast<int>(eye)));
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
		Util::Region::CopyCropToUnprojectedEyes(a_region, projected, a_eyeWidth, a_eyeHeight);
		a_actorBox.active = tracked;
		return tracked;
	}

	/** @brief Pass-to-pass timing ratio above which a calibration result is flagged as unsteady. */
	constexpr float kCalibrationUnstableRatio = 1.5f;

	/** @brief Widest the settings-panel region preview is drawn, in ImGui pixels. */
	constexpr float kRegionPreviewMaxWidth = 400.0f;

	/** @brief Rectangles the preview can draw: a crop and an actor box per eye. */
	constexpr size_t kMaxPreviewRegions = 4;

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
		/** @brief DLSSNR.UIAlpha built from the Masks2 category lane; created only while the material strength is on. */
		std::unique_ptr<WrappedResource> materialAlpha;
		NR::FrameParameters frame;
		std::unique_ptr<Texture2D> resolved, toneData;
		DirectX::SimpleMath::Vector3 position{}, forward{};
	};
	std::array<Eye, 2> eyes;
	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> isolated;
	Util::LazyShader<ID3D11ComputeShader> prepareColor, prepareToneData, compositeColor, materialAlphaShader;
	/** @brief Cbuffer CategoryAlphaCS.hlsl reads: the eye extent, the stereo offset, the softness radius and the six strengths. */
	struct alignas(16) CategoryAlphaData
	{
		uint32_t width, height, eyeOffsetX, edgeSoftness;
		float4 strengthsA;
		float2 strengthsB;
		float2 pad{};
	};
	static_assert(offsetof(CategoryAlphaData, strengthsA) == 16);
	static_assert(offsetof(CategoryAlphaData, strengthsB) == 32);
	static_assert(sizeof(CategoryAlphaData) == 48);
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
		// Per-category tone multipliers, Skin..Landscape in .x; the 16-byte rows mirror
		// ColorTransferCS.hlsl's float4 CategoryStrength[5].
		float4 categoryStrength[5]{};
		uint32_t materialMapEnabled = 0, materialMapMode = 0, materialMapFilter = 0, materialMapStrengthBound = 0;
		float4 materialStrengthsA{};
		float4 materialStrengthsB{};
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
	static_assert(offsetof(ColorTransferData, categoryStrength) == 144);
	static_assert(offsetof(ColorTransferData, materialMapEnabled) == 224);
	static_assert(offsetof(ColorTransferData, materialMapStrengthBound) == 236);
	static_assert(offsetof(ColorTransferData, materialStrengthsA) == 240);
	static_assert(offsetof(ColorTransferData, materialStrengthsB) == 256);
	static_assert(sizeof(ColorTransferData) == 272);
	// ModeValues.hlsli carries these numbers for ColorTransferCS.hlsl's tint.
	static_assert(static_cast<uint32_t>(NR::MaterialMap::Mode::kCategory) == 0);
	static_assert(static_cast<uint32_t>(NR::MaterialMap::Mode::kStrength) == 1);
	std::unique_ptr<ConstantBuffer> colorBuffer;
	std::unique_ptr<ConstantBuffer> categoryAlphaBuffer;
	/** @brief Material-strength switch the persistent eye features were last created with; drives recreation. */
	bool materialStrengthOn = false;
	/** @brief The graded protection is bound for the evaluate now in flight, so a fault belongs to it. */
	bool materialStrengthInFlight = false;
	/** @brief Last evaluate bound the graded protection; reported by the status readout. */
	bool materialStrengthBound = false;
	/** @brief Latched setup failure: the material lane or the alpha texture is unusable for this session. */
	bool materialStrengthDegraded = false;
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
	/** @brief Per-category tone multipliers from the tuning, consumed by TransferColor's cbuffer. */
	std::array<float, 5> categoryToneStrength{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	bool useResolutionMotionScale = true;
	/** @brief Draws the evaluated crop outline into the composite; read from the tuning each NR frame. */
	bool regionOverlay = false;
	/** @brief Material-map tint of the last frame, read from the tuning: the switch, the mode and the category filter. */
	bool materialMapEnabled = false;
	uint32_t materialMapMode = 0, materialMapFilter = NR::MaterialMap::kAllCategories;
	/** @brief Per-category by-material strengths of the last frame, in NeuralRenderingCategory id order. */
	std::array<float, NR::MaterialStrength::kCount> materialMapStrength{};
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
		categoryAlphaBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CategoryAlphaData>(), "NeuralRendering::CategoryAlpha CB");
		runtime.Initialize(interop.Device(), RuntimeDirectory(), globals::state->IsDeveloperMode());
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
		materialStrengthInFlight = materialStrengthBound = false;
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
		ID3D12Resource* resources[5]{ eye.color->resource.get(), eye.depth->resource.get(), eye.motion->resource.get(),
			eye.output->resource.get(), eye.materialAlpha ? eye.materialAlpha->resource.get() : nullptr };
		const D3D12_RESOURCE_STATES states[5]{
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
		};
		D3D12_RESOURCE_BARRIER barriers[5]{};
		uint32_t count = 0;
		for (uint32_t i = 0; i < 5; ++i) {
			if (!resources[i])
				continue;
			barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[count].Transition = { resources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				enter ? D3D12_RESOURCE_STATE_COMMON : states[i], enter ? states[i] : D3D12_RESOURCE_STATE_COMMON };
			++count;
		}
		commands->ResourceBarrier(count, barriers);
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
		for (size_t category = 0; category < categoryToneStrength.size(); ++category)
			data.categoryStrength[category].x = categoryToneStrength[category];
		data.hasToneData = !prepare && NeedsToneData();
		// Never on the Prepare dispatch: that writes NGX's input proxy, which the overlay would corrupt.
		data.regionOverlayEnabled = (!prepare && regionOverlay) ? 1u : 0u;
		data.regionOutlineThickness = kRegionOutlineThicknessPixels;
		data.materialMapEnabled = (!prepare && materialMapEnabled) ? 1u : 0u;
		data.materialMapMode = materialMapMode;
		data.materialMapFilter = materialMapFilter;
		data.materialMapStrengthBound = materialStrengthBound ? 1u : 0u;
		data.materialStrengthsA = float4{ materialMapStrength[NR::MaterialStrength::kNone], materialMapStrength[NR::MaterialStrength::kSkin],
			materialMapStrength[NR::MaterialStrength::kHair], materialMapStrength[NR::MaterialStrength::kEyes] };
		data.materialStrengthsB = float4{ materialMapStrength[NR::MaterialStrength::kFoliage], materialMapStrength[NR::MaterialStrength::kLandscape], 0.0f, 0.0f };
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
		auto* masks2 = Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS_DOWNSCALED].SRV);
		ID3D11ShaderResourceView* inputs[]{ original->srv.get(), prepare ? nullptr : eye.color->srv,
			prepare ? nullptr : eye.output->srv, exposure,
			data.hasToneData ? eye.toneData->srv.get() : nullptr, masks2 };
		ID3D11UnorderedAccessView* outputs[]{ prepare ? eye.color->uav : eye.resolved->uav.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(prepare ? prepareColor.get() : compositeColor.get(), nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	/**
	 * @brief Returns the Masks2 SRV only when it carries the R16G16 material category lane this
	 *        alpha decodes. Any other target would read as None everywhere and protect everything.
	 */
	ID3D11ShaderResourceView* MaterialLane()
	{
		const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS_DOWNSCALED];
		if (!target.SRV)
			return nullptr;
		auto* srv = Util::AsReal(target.SRV);
		D3D11_TEXTURE2D_DESC desc{};
		return Util::GetTexture2DDesc(srv, desc) && desc.Format == DXGI_FORMAT_R16G16_UNORM ? srv : nullptr;
	}

	/**
	 * @brief Builds each eye's DLSSNR.UIAlpha from the Masks2 category lane for this frame.
	 * @param masks2 The deferred Masks2 SRV the categories are decoded from.
	 * @param strengths The per-category protection and edge softness CategoryAlphaCS.hlsl applies.
	 * @return False on a shader or texture failure; the caller then binds no protection.
	 */
	bool BuildMaterialAlpha(ID3D11ShaderResourceView* masks2, const NR::MaterialStrength::Values& strengths)
	{
		CS_GPU_PASS("Upscaling::NRMaterialAlpha");
		auto* shader = materialAlphaShader.Get(L"Data/Shaders/Upscaling/NeuralRendering/CategoryAlphaCS.hlsl",
			{}, "cs_5_0", "main", "NeuralRendering::CategoryAlpha CS");
		if (!shader)
			return false;
		try {
			for (uint32_t i = 0; i < eyeCount; ++i) {
				auto& eye = eyes[i];
				if (!eye.materialAlpha) {
					const auto name = eyeCount > 1 ? std::format("NeuralRendering::MaterialAlpha{}", i) : std::string("NeuralRendering::MaterialAlpha");
					eye.materialAlpha = interop.CreateTexture(width, height, DXGI_FORMAT_R8_UNORM, name);
				}
				context->ClearState();
				categoryAlphaBuffer->Update(CategoryAlphaData{ width, height, i * width, strengths.edgeSoftness,
					{ strengths.strength[NR::MaterialStrength::kNone], strengths.strength[NR::MaterialStrength::kSkin],
						strengths.strength[NR::MaterialStrength::kHair], strengths.strength[NR::MaterialStrength::kEyes] },
					{ strengths.strength[NR::MaterialStrength::kFoliage], strengths.strength[NR::MaterialStrength::kLandscape] } });
				auto buffer = categoryAlphaBuffer->CB();
				context->CSSetConstantBuffers(0, 1, &buffer);
				context->CSSetShaderResources(0, 1, &masks2);
				auto* output = eye.materialAlpha->uav;
				context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
				context->CSSetShader(shader, nullptr, 0);
				context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
				context->ClearState();
			}
		} catch (...) {
			if (interop.DeviceRemoved())
				throw;
			return false;
		}
		return true;
	}

	bool Draw(ID3D11Texture2D* color, ID3D11ShaderResourceView* const* inputs, ID3D11ComputeShader* shader, uint32_t reset, bool materialStrengthWanted, const NR::MaterialStrength::Values& materialStrengths, const NR::Tuning& tuning, NR::Diagnostics::Frame& diagnostic, NR::Diagnostics& diagnostics)
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
		materialStrengthInFlight = materialStrengthBound = false;
		if (materialStrengthWanted && !materialStrengthDegraded) {
			auto* lane = MaterialLane();
			if (!lane) {
				materialStrengthDegraded = true;
				logger::warn("[NeuralRendering] material strength unavailable: the deferred material lane is not present, so the whole frame is processed");
			} else if (BuildMaterialAlpha(lane, materialStrengths)) {
				materialStrengthInFlight = materialStrengthBound = true;
			} else {
				materialStrengthDegraded = true;
				logger::warn("[NeuralRendering] material strength unavailable: its shader or texture could not be created, so the whole frame is processed");
			}
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
					const NR::ProtectionResources protection{
						materialStrengthInFlight && eye.materialAlpha ? eye.materialAlpha->resource.get() : nullptr,
						// A protected pixel is restored from the NR input itself, so the alpha and the
						// backbuffer share the proxy colour domain the model works in.
						materialStrengthInFlight ? eye.color->resource.get() : nullptr
					};
					success = runtime.Evaluate(commands, i, eye.color->resource.get(), eye.depth->resource.get(),
						eye.motion->resource.get(), eye.output->resource.get(), protection,
						width, height, guides, eye.frame, tuning);
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
		if (success)
			materialStrengthInFlight = false;
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
	stl::write_thunk_call<MainUpdate_UpdateRegionOfInterest>(Util::MainUpdateCallSite());
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
			if (NR::CropCalibration::ConsumeCapture(globals::profiler->GetCapturedGpuFrameCount(), lastCalibrationGpuFrame)) {
				for (const auto& timer : globals::profiler->GetResults()) {
					if (timer.valid && timer.activeGpu && timer.name == "Upscaling::NREvaluate")
						gpuMs = timer.gpuTimeMs;
				}
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
			const auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const bool playerIsSeparateFromCamera = !globals::game::isVR && playerCamera && playerCamera->IsInThirdPerson();
			std::vector<RegionCandidate> candidates;
			Util::ForEachLoadedActor([&](RE::Actor* a_actor) {
				if (!a_actor || !a_actor->Is3DLoaded() || (a_actor == globals::game::player && !playerIsSeparateFromCamera))
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
		Util::Region::MatchEyeSizes(region, eyeWidth, eyeHeight);
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

const char* NeuralRendering::RegionSourceName(RegionSource a_source)
{
	switch (a_source) {
	case RegionSource::kActor:
		return "actor";
	case RegionSource::kFovea:
		return "fovea";
	case RegionSource::kBoth:
		return "both";
	default:
		return "none";
	}
}

bool NeuralRendering::DialogueOpen()
{
	return globals::game::ui && globals::game::ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
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
	snapshot.materialStrengthActive = materialStrengthActive.load(std::memory_order_relaxed);
	snapshot.materialStrengthAvailable = materialStrengthAvailable.load(std::memory_order_relaxed);
	for (size_t i = 0; i < snapshot.materialStrength.size(); ++i)
		snapshot.materialStrength[i] = materialStrengthValues[i].load(std::memory_order_relaxed);
	snapshot.materialEdgeSoftness = materialEdgeSoftness.load(std::memory_order_relaxed);
	return snapshot;
}

NR::RuntimeAvailability NeuralRendering::GetRuntimeAvailability() const
{
	return NR::InspectRuntime(RuntimeDirectory());
}

void NeuralRendering::DrawRuntimeDiagnostics() const
{
	// The Streamline table lists this file's version like any other DLL in that folder; the
	// verdict is what says whether that version is one the pass will actually load.
	const auto availability = GetRuntimeAvailability();
	if (availability.Ready()) {
		Util::Text::Success(T(TKEY("runtime_validated"), "Neural Rendering runtime: %s %s is a validated build."), NR::kRuntimeFileName, availability.version.c_str());
		return;
	}
	Util::Text::WrappedWarning(T(TKEY("runtime_unavailable"), "Neural Rendering runtime: %s"), availability.reason.c_str());
	if (availability.AllowsLoad(globals::state && globals::state->IsDeveloperMode()))
		Util::Text::Disabled("%s", T(TKEY("runtime_developer_load"), "Loading this build because developer mode is on; its output is unverified."));
	else
		Util::Text::Disabled("%s", RuntimeFixHint(availability.state));
}

namespace
{
	constexpr uint32_t kMaterialChecklistColumns = 3;
	constexpr float kStrengthLegendWidthFraction = 0.6f;
	using MaterialSelection = std::array<uint8_t, NR::MaterialMap::kBits>;

	/** @brief NeuralRenderingCategory ids in the order the material checklists list them. */
	constexpr std::array<uint32_t, NR::MaterialMap::kBits> kMaterialListOrder{
		NR::MaterialStrength::kSkin, NR::MaterialStrength::kHair, NR::MaterialStrength::kEyes,
		NR::MaterialStrength::kFoliage, NR::MaterialStrength::kLandscape, NR::MaterialStrength::kNone
	};

	ImU32 ToColor(const NR::MaterialMap::Color& color)
	{
		return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, 1.0f));
	}

	/** @brief Draws one checkbox per material, three to a row, optionally led by its map colour; returns true when one changed. */
	bool DrawMaterialChecklist(MaterialSelection& selected, bool colourSwatches)
	{
		const std::array<const char*, NR::MaterialMap::kBits> labels{
			T(TKEY("category_skin"), "Skin"), T(TKEY("category_hair"), "Hair"), T(TKEY("category_eyes"), "Eyes"),
			T(TKEY("category_foliage"), "Foliage"), T(TKEY("category_landscape"), "Landscape"), T(TKEY("material_other"), "Everything Else")
		};
		bool changed = false;
		for (uint32_t i = 0; i < selected.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			if (i % kMaterialChecklistColumns != 0)
				ImGui::SameLine();
			if (colourSwatches) {
				const auto& color = NR::MaterialMap::kColors[kMaterialListOrder[i]];
				ImGui::ColorButton("##swatch", ImVec4(color.r, color.g, color.b, 1.0f),
					ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
					ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
				ImGui::SameLine();
			}
			bool value = selected[i] != 0;
			if (ImGui::Checkbox(labels[i], &value)) {
				selected[i] = value ? 1 : 0;
				changed = true;
			}
			ImGui::PopID();
		}
		return changed;
	}

	/** @brief Select All and Select None for a material checklist; returns true when a button changed it. */
	bool DrawMaterialSelectionButtons(MaterialSelection& selected)
	{
		const auto before = selected;
		Util::DrawSelectionButtons(selected,
			T("feature.scene_manager.action.select_all", "Select All"),
			T("feature.scene_manager.action.select_none", "Select None"));
		return selected != before;
	}

	/** @brief Draws the colour bar the Strength map mode reads against, 0 at the left to 1 at the right. */
	void DrawStrengthLegend()
	{
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x * kStrengthLegendWidthFraction;
		const float height = ImGui::GetTextLineHeight();
		const auto& ramp = NR::MaterialMap::kStrengthRamp;
		const float segment = width / static_cast<float>(std::size(ramp) - 1);
		auto* drawList = ImGui::GetWindowDrawList();
		for (size_t i = 0; i + 1 < std::size(ramp); ++i) {
			const float left = origin.x + segment * static_cast<float>(i);
			drawList->AddRectFilledMultiColor(ImVec2(left, origin.y), ImVec2(left + segment, origin.y + height),
				ToColor(ramp[i]), ToColor(ramp[i + 1]), ToColor(ramp[i + 1]), ToColor(ramp[i]));
		}
		ImGui::Dummy(ImVec2(width, height));
		ImGui::TextUnformatted(T(TKEY("material_map_strength_legend"), "Strength: 0 at the left, 1 at the right"));
	}

	/** @brief Draws the by-material selection and the material map controls; returns true when any value changed. */
	bool DrawMaterialControls(NR::Tuning& tuning, bool materialStrengthAvailable)
	{
		bool changed = false;
		ImGui::TextUnformatted(T(TKEY("material_apply_to"), "Apply Neural Rendering To"));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("material_apply_to_tooltip"),
				"Choose the materials Neural Rendering is applied to, using the labels the deferred pass writes. All selected is the normal behaviour. It changes where the effect shows, not how much GPU time it costs, and needs the deferred pass. Fine-Tune Strengths sets partial amounts."));
		ImGui::PushID("materialSelection");
		MaterialSelection selected{};
		for (uint32_t i = 0; i < selected.size(); ++i)
			selected[i] = tuning.MaterialSelected(kMaterialListOrder[i]) ? 1 : 0;
		const auto before = selected;
		DrawMaterialSelectionButtons(selected);
		ImGui::SameLine();
		if (ImGui::SmallButton(T(TKEY("material_characters_only"), "Characters Only")))
			for (uint32_t i = 0; i < selected.size(); ++i)
				selected[i] = NR::MaterialStrength::kCharactersOnly[kMaterialListOrder[i]] > NR::MaterialStrength::kMinStrength ? 1 : 0;
		DrawMaterialChecklist(selected, false);
		ImGui::PopID();
		for (uint32_t i = 0; i < selected.size(); ++i) {
			if (selected[i] != before[i]) {
				tuning.SetMaterialSelected(kMaterialListOrder[i], selected[i] != 0);
				changed = true;
			}
		}
		if (ImGui::TreeNodeEx(T(TKEY("material_strengths"), "Fine-Tune Strengths"), ImGuiTreeNodeFlags_None)) {
			ImGui::PushID("materialStrength");
			bool strengthChanged = false;
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_skin"), "Skin"), &tuning.strengthSkin, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_hair"), "Hair"), &tuning.strengthHair, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_eyes"), "Eyes"), &tuning.strengthEyes, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_foliage"), "Foliage"), &tuning.strengthFoliage, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("category_landscape"), "Landscape"), &tuning.strengthLandscape, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			strengthChanged |= ImGui::SliderFloat(T(TKEY("material_other"), "Everything Else"), &tuning.strengthOther, NR::MaterialStrength::kMinStrength, NR::MaterialStrength::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (strengthChanged) {
				tuning.SyncMaterialSwitch();
				changed = true;
			}
			int edgeSoftness = static_cast<int>(tuning.strengthEdgeSoftness);
			if (ImGui::SliderInt(T(TKEY("edge_softness"), "Edge Softness"), &edgeSoftness, 0, static_cast<int>(NR::MaterialStrength::kMaxEdgeSoftness))) {
				tuning.strengthEdgeSoftness = static_cast<uint32_t>(edgeSoftness);
				changed = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("edge_softness_tooltip"),
					"Blends each material's strength into its neighbours over this many pixels; 0 keeps a hard boundary between materials."));
			ImGui::PopID();
			ImGui::TreePop();
		}
		if (tuning.materialStrength && !materialStrengthAvailable)
			Util::Text::WrappedWarning("%s", T(TKEY("material_unavailable"), "Neural Rendering by material is unavailable this session, so the whole frame is processed. Check the log; restarting the game retries."));
		if (ImGui::Checkbox(T(TKEY("material_map"), "Show Material Map"), &tuning.showMaterialMap))
			changed = true;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("material_map_tooltip"),
				"Tints each pixel by the material the deferred pass labelled it, so a character the classification mislabels is visible. The filter picks which materials show; the mode picks the category's colour or a colour ramp of the strength the by-material selection gives that pixel, read at the pixel without Edge Softness. Needs the deferred pass; unlabelled pixels follow Everything Else."));
		ImGui::BeginDisabled(!tuning.showMaterialMap);
		ImGui::PushID("materialMap");
		int materialMapMode = static_cast<int>(std::min(tuning.materialMapMode, NR::MaterialMap::kMaxMode));
		const std::array<const char*, NR::MaterialMap::kMaxMode + 1> materialMapModeLabels{
			T(TKEY("material_map_category"), "Category colours"),
			T(TKEY("material_map_strength"), "Strength"),
		};
		if (ImGui::Combo(T(TKEY("material_map_mode"), "Map Mode"), &materialMapMode, materialMapModeLabels.data(), static_cast<int>(materialMapModeLabels.size()))) {
			tuning.materialMapMode = static_cast<uint32_t>(materialMapMode);
			changed = true;
		}
		if (tuning.materialMapMode == static_cast<uint32_t>(NR::MaterialMap::Mode::kStrength))
			DrawStrengthLegend();
		ImGui::TextUnformatted(T(TKEY("material_map_filter"), "Show"));
		MaterialSelection shown{};
		for (uint32_t i = 0; i < shown.size(); ++i)
			shown[i] = NR::MaterialMap::Contains(tuning.materialMapFilter, kMaterialListOrder[i]) ? 1 : 0;
		bool filterChanged = DrawMaterialSelectionButtons(shown);
		filterChanged |= DrawMaterialChecklist(shown, true);
		if (filterChanged) {
			for (uint32_t i = 0; i < shown.size(); ++i)
				tuning.materialMapFilter = NR::MaterialMap::Set(tuning.materialMapFilter, kMaterialListOrder[i], shown[i] != 0);
			changed = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("material_map_filter_tooltip"),
				"Each swatch is the colour that material is drawn in. A material that is off keeps its normal pixels."));
		ImGui::PopID();
		ImGui::EndDisabled();
		return changed;
	}
}

void NeuralRendering::DrawSettings(bool& enabled, NR::Context::DialogueProfile& dialogue, NR::Tuning& tuning)
{
	ImGui::PushID("NeuralRendering");
	const auto availability = GetRuntimeAvailability();
	const bool developerMode = globals::state->IsDeveloperMode();
	// An unavailable runtime must not strand a feature that is already on, so only the switch
	// from off is locked. The verdict is re-read when the file changes, so installing a build
	// mid-session unblocks the toggle without a restart.
	const bool loadable = availability.AllowsLoad(developerMode);
	const bool lockEnable = !loadable && !enabled;
	ImGui::BeginDisabled(lockEnable);
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable Neural Rendering"), &enabled))
		retryRequested = resetHistory = true;
	ImGui::EndDisabled();
	if (lockEnable) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(availability.reason.c_str());
			ImGui::TextUnformatted(RuntimeFixHint(availability.state));
		}
	}
	if (!availability.Ready()) {
		if (loadable)
			Util::Text::WrappedWarning("%s", T(TKEY("runtime_developer_load"), "Loading this build because developer mode is on; its output is unverified."));
		else
			Util::Text::Disabled("%s", RuntimeFixHint(availability.state));
	}
	ImGui::TextWrapped("%s", T(TKEY("description"),
								 "One display-referred NR proxy pass at eye render resolution, composed back into scene-linear HDR before DLSS/FSR and frame-generation capture. Requires an NR-capable NVIDIA GPU and a validated 310.8 runtime build."));
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
	if (ImGui::CollapsingHeader(T(TKEY("category_tone"), "Category Tone Strengths"))) {
		ImGui::PushID("categoryTone");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("category_tone_tooltip"),
				"Scales Neural Rendering's tone edit per material: 0 leaves a material's brightness untouched, 1 is the normal edit, and 2 doubles it. Materials the deferred buffer does not label are unaffected."));
		changed |= ImGui::SliderFloat(T(TKEY("category_skin"), "Skin"), &tuning.skinToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_hair"), "Hair"), &tuning.hairToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_eyes"), "Eyes"), &tuning.eyeToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_foliage"), "Foliage"), &tuning.foliageToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderFloat(T(TKEY("category_landscape"), "Landscape"), &tuning.landscapeToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::PopID();
	}
	changed |= DrawMaterialControls(tuning, materialStrengthAvailable.load(std::memory_order_relaxed));
	if (ImGui::Checkbox(T(TKEY("region_of_interest"), "Limit to Tracked Actor"), &tuning.regionOfInterest)) {
		changed = true;
		resetHistory = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("region_of_interest_tooltip"),
			"Restricts Neural Rendering to a crop around the most prominent visible character, the one covering the most of the view with the centre favoured, and leaves the rest of the frame at pre-NR quality. Costs less GPU time when a character is on screen."));
	if (globals::game::isVR) {
		if (ImGui::Checkbox(T(TKEY("region_follow_foveation"), "Follow Foveation"), &tuning.regionFollowFoveation)) {
			changed = true;
			resetHistory = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("region_follow_foveation_tooltip"),
				"With Foveation on, evaluates only the foveated region, narrowed to the tracked character's crop when that is set, so the periphery keeps its pre-Neural-Rendering content. The upscaler already replaces that periphery with the cheap stretched view."));
	}
	// The crop controls act only through a tracked crop, so they follow the toggle and are greyed
	// out without it instead of accepting edits that the pass ignores.
	const bool cropDisabled = !tuning.regionOfInterest;
	ImGui::BeginDisabled(cropDisabled);
	if (ImGui::Checkbox(T(TKEY("crop_group"), "Include Nearby Characters"), &tuning.regionGroup)) {
		changed = true;
		resetHistory = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("crop_group_tooltip"),
			"Grows the crop to also cover the next most prominent characters while it stays under half the view, so a group is evaluated together. Off tracks one character."));
	if (ImGui::Checkbox(T(TKEY("region_overlay"), "Show Region Overlay"), &tuning.regionOverlay))
		changed = true;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("region_overlay_tooltip"),
			"Draws the evaluated crop: a green outline in the game frame and the same rectangle over the preview below. Only meaningful with Limit to Tracked Actor on, and it draws nothing while no character is tracked, since the whole frame is evaluated then."));
	ImGui::EndDisabled();
	if (ImGui::CollapsingHeader(T(TKEY("dialogue"), "Dialogue"))) {
		ImGui::PushID("dialogueProfile");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("dialogue_tooltip"),
				"Changes only what Neural Rendering does while a dialogue is open. The default changes nothing: the normal frame and dialogue both follow the settings above."));
		ImGui::Checkbox(T(TKEY("dialogue_only"), "Only in dialogue"), &dialogue.onlyInDialogue);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("dialogue_only_tooltip"),
				"Evaluates Neural Rendering only while a dialogue is open, and leaves the frame's rendering untouched the rest of the time. The pass stays initialized, so opening a dialogue resumes it without a rebuild."));
		int scopeItem = static_cast<int>(std::min(dialogue.scope, NR::Context::kMaxScope));
		const std::array<const char*, 4> dialogueScopeLabels{
			T(TKEY("dialogue_same_as_normal"), "Same as normal"),
			T(TKEY("material_scope_everything"), "Everything"),
			T(TKEY("material_scope_characters"), "Skin, hair and eyes"),
			T(TKEY("material_scope_characters_foliage"), "Skin, hair, eyes and foliage"),
		};
		if (ImGui::Combo(T(TKEY("dialogue_scope"), "In dialogue, apply Neural Rendering to"), &scopeItem, dialogueScopeLabels.data(), static_cast<int>(dialogueScopeLabels.size())))
			dialogue.scope = static_cast<NR::Context::ScopeOverride>(scopeItem);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("dialogue_scope_tooltip"),
				"What Neural Rendering applies to while a dialogue is open. Choosing a scope keeps the by-material lane bound the whole time, at a cost of about 0.04 ms, so entering and leaving dialogue never rebuilds Neural Rendering."));
		ImGui::BeginDisabled(cropDisabled);
		int regionItem = static_cast<int>(std::min(dialogue.region, NR::Context::kMaxRegion));
		const std::array<const char*, 2> dialogueRegionLabels{
			T(TKEY("dialogue_same_as_normal"), "Same as normal"),
			T(TKEY("dialogue_region_full"), "Full frame"),
		};
		if (ImGui::Combo(T(TKEY("dialogue_region"), "In dialogue, crop"), &regionItem, dialogueRegionLabels.data(), static_cast<int>(dialogueRegionLabels.size())))
			dialogue.region = static_cast<NR::Context::RegionOverride>(regionItem);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("dialogue_region_tooltip"),
				"Full frame evaluates the whole view during dialogue instead of the character's crop. It needs Limit to Tracked Actor on; Same as normal follows that setting."));
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	if (ImGui::Button(T(TKEY("restore_defaults"), "Restore NR Defaults"))) {
		tuning = {};
		changed = recreateTuning = true;
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("reset_history"), "Reset NR History")))
		resetHistory = true;
	if (enabled && ImGui::Button(T(TKEY("retry"), "Retry NR")))
		RequestRetry();
	if (globals::state->IsDeveloperMode() && ImGui::TreeNodeEx(T(TKEY("developer"), "Developer"), ImGuiTreeNodeFlags_None)) {
		ImGui::SeparatorText(T(TKEY("developer_model"), "Model input"));
		const bool autoMaskChanged = ImGui::Checkbox(T(TKEY("use_auto_mask"), "Use NGX Automatic Mask"), &tuning.useAutoMask);
		changed |= autoMaskChanged;
		recreateTuning |= autoMaskChanged;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("use_auto_mask_tooltip"),
				"NVIDIA's built-in automatic mask. On is the normal setting; turning it off with no mask supplied is an untested configuration."));
		ImGui::SeparatorText(T(TKEY("developer_crop"), "Crop"));
		ImGui::BeginDisabled(cropDisabled);
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
		ImGui::EndDisabled();
		// The sweep forces its own centred crops, so it stays usable without a tracked actor.
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
		ImGui::SeparatorText(T(TKEY("developer_diagnostics"), "Diagnostics"));
		if (ImGui::Checkbox("Use resolution-scaled NR motion", &impl->useResolutionMotionScale))
			resetHistory = true;
		diagnostics.DrawSettings();
		ImGui::TreePop();
	}
	if (changed)
		tuning.Sanitize();
	if (recreateTuning)
		recreate = resetHistory = true;
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
	const float maxWidth = std::min(kRegionPreviewMaxWidth, ImGui::GetContentRegionAvail().x);
	const float aspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
	const ImVec2 imageSize(maxWidth, maxWidth / aspect);
	const ImVec2 imageMin = ImGui::GetCursorScreenPos();
	Util::Subrect::ImageOpaque(preview, imageSize);
	std::array<Util::RegionOverlay::Region, kMaxPreviewRegions> rects{};
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

void NeuralRendering::DrawBeforeUpscaling(bool enabled, const NR::Context::DialogueProfile& dialogue, const NR::Tuning& tuning, uint32_t target, float2 renderSize)
{
	using Outcome = NR::Diagnostics::Outcome;
	const auto kind = NR::Context::ResolveContext(DialogueOpen());
	const bool suspended = NR::Context::Suspended(dialogue, kind);
	const bool resumed = NR::ResumesFromSuspend(suspended, wasSuspended);
	auto& diagnostic = diagnostics.BeginHook(globals::state->frameCount, target);
	const auto action = NR::DecideFrame({ enabled, suspended, globals::state->worldRenderedThisFrame, impl->failed,
		retryRequested.load(), impl->ready, impl->lastFrame, globals::state->frameCount });
	if (resumed)
		resetHistory = true;
	if (NR::Context::ContextChangeNeedsReset(dialogue, lastContext, kind))
		resetHistory = true;
	lastContext = kind;
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
	if (action == NR::FrameAction::Suspend) {
		diagnostic.outcome = Outcome::Suspended;
		if (publishedState.load(std::memory_order_relaxed) != Status::State::kSuspended) {
			logger::debug("[NeuralRendering] Suspended: waiting for dialogue");
			PublishStatus(Status::State::kSuspended, T(TKEY("status_waiting_dialogue"), "Waiting for dialogue"));
		}
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
	const auto effective = NR::Context::EffectiveTuning(tuning, dialogue, kind);
	const auto materialStrengths = effective.MaterialStrengths();
	const auto publishMaterialStrength = [this, &materialStrengths] {
		materialStrengthActive.store(impl->materialStrengthBound, std::memory_order_relaxed);
		materialStrengthAvailable.store(!impl->materialStrengthDegraded && !materialStrengthRejected.load(std::memory_order_relaxed), std::memory_order_relaxed);
		for (size_t i = 0; i < materialStrengths.strength.size(); ++i)
			materialStrengthValues[i].store(materialStrengths.strength[i], std::memory_order_relaxed);
		materialEdgeSoftness.store(materialStrengths.edgeSoftness, std::memory_order_relaxed);
	};
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
			work.materialAlphaShader.Reset();
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
		const bool materialStrengthWanted = effective.materialStrength && !materialStrengthRejected.load(std::memory_order_relaxed);
		// Feature 18 latches the UIAlpha binding at creation, so the lane is bound from the settings
		// alone: only a change there rebuilds the eye features, never a dialogue open or close.
		if (materialStrengthWanted != work.materialStrengthOn) {
			recreate = resetHistory = true;
			work.materialStrengthOn = materialStrengthWanted;
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
		auto boundedTuning = effective;
		boundedTuning.Sanitize();
		const bool calibrationRunning = GetCalibration().state == NR::CropCalibration::State::kRunning;
		const bool cropActive = boundedTuning.regionOfInterest || calibrationRunning;
		work.region = cropActive ? GetRegionOfInterest() : Util::Region::StereoRegion{};
		work.actorBox = cropActive ? GetActorBox() : Util::Region::StereoRegion{};
		const bool actorCrop = work.region.active && !calibrationRunning;
		const bool foveatedRouteSuspended = state->IsMainOrLoadingMenuOpen();
		bool foveaClip = false;
		if (boundedTuning.regionFollowFoveation && !calibrationRunning && !foveatedRouteSuspended) {
			const auto& foveated = globals::features::upscaling.foveatedRender;
			Util::Subrect::UVRegion leftUV, rightUV;
			if (foveated.GetClipUV(0, leftUV) && foveated.GetClipUV(1, rightUV)) {
				const auto clip = NR::FoveaClip::BuildClip(std::array<Util::Subrect::UVRegion, 2>{ leftUV, rightUV }, w, h);
				if (clip.active) {
					Util::Region::ClipRegion(work.region, clip);
					NR::FoveaClip::ClipSubject(work.actorBox, clip);
					foveaClip = true;
				}
			}
		}
		auto source = RegionSource::kNone;
		if (actorCrop)
			source = foveaClip ? RegionSource::kBoth : RegionSource::kActor;
		else if (foveaClip)
			source = RegionSource::kFovea;
		regionSource.store(static_cast<uint32_t>(source), std::memory_order_relaxed);
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
		work.categoryToneStrength = { boundedTuning.skinToneStrength, boundedTuning.hairToneStrength,
			boundedTuning.eyeToneStrength, boundedTuning.foliageToneStrength, boundedTuning.landscapeToneStrength };
		work.regionOverlay = boundedTuning.regionOverlay;
		work.materialMapEnabled = boundedTuning.showMaterialMap;
		work.materialMapMode = boundedTuning.materialMapMode;
		work.materialMapFilter = boundedTuning.materialMapFilter;
		work.materialMapStrength = materialStrengths.strength;
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
		const bool processed = work.Draw(color, inputs.data(), shader, reset, materialStrengthWanted, materialStrengths, boundedTuning, diagnostic, diagnostics);
		publishMaterialStrength();
		if (!processed)
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
		const bool strengthHandled = RevertMaterialStrengthOnFailure(winrt::to_string(error.message()).c_str());
		publishMaterialStrength();
		if (strengthHandled)
			return;
		LatchFailure();
		PublishFailure(winrt::to_string(error.message()));
		logger::error("[NeuralRendering] D3D initialization/dispatch failed: 0x{:08X}", static_cast<uint32_t>(error.code().value));
	} catch (const std::exception& error) {
		diagnostic.outcome = Outcome::Error;
		const bool strengthHandled = RevertMaterialStrengthOnFailure(error.what());
		publishMaterialStrength();
		if (strengthHandled)
			return;
		LatchFailure();
		PublishFailure(error.what());
		logger::error("[NeuralRendering] {}", error.what());
	}
}

bool NeuralRendering::RevertMaterialStrengthOnFailure(const char* detail)
{
	if (!impl->materialStrengthInFlight || impl->interop.DeviceRemoved())
		return false;
	impl->materialStrengthInFlight = false;
	impl->materialStrengthBound = false;
	materialStrengthRejected.store(true, std::memory_order_relaxed);
	logger::warn("[NeuralRendering] material strength rejected ({}); Neural Rendering by material is off for this session", detail);
	return true;
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
