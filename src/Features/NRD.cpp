#include "NRD.h"
#include "NRDPortStereo.h"
#include "ScreenSpaceGI.h"
#include "ScreenSpaceReflections.h"

#include "Deferred.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Upscaling.h"
#include "Util.h"
#include "GpuPass.h"
#include "Utils/MathUtils.h"

#define I18N_KEY_PREFIX "feature.nrd."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NRD::Settings,
	Enabled)

////////////////////////////////////////////////////////////////////////////////////

void NRD::RestoreDefaultSettings()
{
	const bool wasEnabled = settings.Enabled;
	settings = {};
	if (wasEnabled != settings.Enabled) {
		globals::features::screenSpaceGI.QueueHistoryReset();
		globals::features::screenSpaceReflections.QueueHistoryReset();
	}
}

void NRD::DrawSettings()
{
	ImGui::TextWrapped("%s", T(TKEY("description_text"),
								 "NRD provides shared denoising infrastructure for screen-space "
								 "radiance signals. It is consumed by features such as Screen Space "
								 "GI and Screen Space Reflections; toggle those features to control "
								 "whether denoising actually runs."));

	ImGui::Separator();
	if (ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled)) {
		globals::features::screenSpaceGI.QueueHistoryReset();
		globals::features::screenSpaceReflections.QueueHistoryReset();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enabled_tooltip"),
							  "When disabled, NRD skips the guide preparation pass and consumer "
							  "features that depend on denoising will fall back to undenoised input."));
	}

	if (ImGui::TreeNode(T(TKEY("buffer_viewer"), "Buffer Viewer"))) {
		static float debugRescale = .3f;
		ImGui::SliderFloat(T(TKEY("view_resize"), "View Resize"), &debugRescale, 0.f, 1.f);

		if (!globals::game::isVR) {
			DrawBufferViewer(debugRescale);
		} else {
			if (ImGui::TreeNode(T("feature.nrd.left_eye", "Left eye"))) {
				DrawBufferViewer(debugRescale);
				ImGui::TreePop();
			}
			if (ImGui::TreeNode(T("feature.nrd.right_eye", "Right eye"))) {
				if (const auto* right = NRDPort::RightNRDDebug())
					right->DrawBufferViewer(debugRescale);
				else
					ImGui::TextUnformatted(T("feature.nrd.eye_buffers_unavailable", "Right-eye buffers are not allocated yet."));
				ImGui::TreePop();
			}
		}

		ImGui::TreePop();
	}
}

void NRD::DrawBufferViewer(float debugRescale) const
{
	if (texNRDViewZ)
		BUFFER_VIEWER_NODE(texNRDViewZ, debugRescale)
	if (texNRDNormalRoughness)
		BUFFER_VIEWER_NODE(texNRDNormalRoughness, debugRescale)
	if (texNRDMV)
		BUFFER_VIEWER_NODE(texNRDMV, debugRescale)
}

void NRD::LoadSettings(json& o_json)
{
	settings = o_json;
}

void NRD::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NRD::SetupResources()
{
	commonSettingsValidThisFrame = false;
	guidesReadyThisFrame = false;
	hasCommonFrameHistory = false;
	lastCommonGameFrame = 0;
	prevResourceSize[0] = prevResourceSize[1] = 0;
	prevRectSize[0] = prevRectSize[1] = 0;
	resourceExtent[0] = resourceExtent[1] = 0;
	texNRDMV = nullptr;
	texNRDViewZ = nullptr;
	texNRDNormalRoughness = nullptr;

	CompileComputeShaders();
}

bool NRD::GetResourceExtent(uint32_t& a_width, uint32_t& a_height) const
{
	if (!texNRDViewZ || !resourceExtent[0] || !resourceExtent[1])
		return false;
	a_width = resourceExtent[0];
	a_height = resourceExtent[1];
	return true;
}

void NRD::EnsureResourceExtent(bool a_fullExtent)
{
	auto mainTex = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!mainTex.texture)
		return;
	D3D11_TEXTURE2D_DESC mainDesc;
	Util::AsReal(mainTex.texture)->GetDesc(&mainDesc);

	(void)a_fullExtent;
	NRDPort::EyeTextureDesc(mainDesc);
	uint32_t width = mainDesc.Width;
	uint32_t height = mainDesc.Height;
	if (texNRDViewZ && width == resourceExtent[0] && height == resourceExtent[1])
		return;

	D3D11_TEXTURE2D_DESC texDesc{
		.Width = width,
		.Height = height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16_FLOAT,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	guidesReadyThisFrame = false;
	try {
		texNRDMV = eastl::make_unique<Texture2D>(texDesc, "NRD::MV");
		texNRDMV->CreateSRV(srvDesc);
		texNRDMV->CreateUAV(uavDesc);

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texNRDViewZ = eastl::make_unique<Texture2D>(texDesc, "NRD::ViewZ");
		texNRDViewZ->CreateSRV(srvDesc);
		texNRDViewZ->CreateUAV(uavDesc);

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
		texNRDNormalRoughness = eastl::make_unique<Texture2D>(texDesc, "NRD::NormalRoughness");
		texNRDNormalRoughness->CreateSRV(srvDesc);
		texNRDNormalRoughness->CreateUAV(uavDesc);
	} catch (const std::exception& e) {
		logger::error("[NRD] Failed to create the guide textures ({})", e.what());
		texNRDMV = nullptr;
		texNRDViewZ = nullptr;
		texNRDNormalRoughness = nullptr;
		resourceExtent[0] = resourceExtent[1] = 0;
		guideResourcesFailed = true;
		return;
	}
	guideResourcesFailed = false;
	resourceExtent[0] = width;
	resourceExtent[1] = height;
}

void NRD::ClearShaderCache()
{
	NRDPort::InvalidateStereo();
	prepareNRDGuidesCompute = nullptr;
	CompileComputeShaders();
}

void NRD::CompileComputeShaders()
{
	std::vector<std::pair<const char*, const char*>> defines;

	auto path = std::filesystem::path("Data\\Shaders\\NRD") / "prepareNRDGuides.cs.hlsl";
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0")))
		prepareNRDGuidesCompute.attach(rawPtr);
	guideShaderFailed = !prepareNRDGuidesCompute;
}

void NRD::PrepareGuides(bool a_requested, bool a_fullExtent)
{
	commonSettingsValidThisFrame = false;
	guidesReadyThisFrame = false;
	if (!a_requested || !settings.Enabled) return;
	EnsureResourceExtent(a_fullExtent);

	if (!a_requested || !settings.Enabled || !prepareNRDGuidesCompute || !texNRDViewZ || !texNRDNormalRoughness || !texNRDMV)
		return;

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;

	ZoneScoped;
	CS_GPU_PASS("NRD::PrepareGuides");


	auto normal = rts[NORMALROUGHNESS];

	float2 dynres = NRDPort::ActiveSize();

	NRDPort::BindEye();
	globals::state->BindSharedDataCS(context);

	std::array<ID3D11ShaderResourceView*, 2> guideSRVs = {
		NRDPort::Crop(NRDPort::Input::Depth, Util::GetCurrentSceneDepthSRV(false)),
		NRDPort::Crop(NRDPort::Input::Normal, Util::AsReal(normal.SRV))
	};
	std::array<ID3D11UnorderedAccessView*, 2> guideUAVs = {
		texNRDViewZ->uav.get(),
		texNRDNormalRoughness->uav.get()
	};

	context->CSSetShaderResources(0, (uint)guideSRVs.size(), guideSRVs.data());
	context->CSSetUnorderedAccessViews(0, (uint)guideUAVs.size(), guideUAVs.data(), nullptr);
	context->CSSetShader(prepareNRDGuidesCompute.get(), nullptr, 0);
	context->Dispatch(((uint)dynres.x + 7) / 8, ((uint)dynres.y + 7) / 8, 1);

	std::array<ID3D11ShaderResourceView*, 2> nullSRVs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 2> nullUAVs = { nullptr };
	context->CSSetShaderResources(0, (uint)nullSRVs.size(), nullSRVs.data());
	context->CSSetUnorderedAccessViews(0, (uint)nullUAVs.size(), nullUAVs.data(), nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	// Motion Vector is used as both SRV and UAV by ReBLUR; snapshot the game's
	// MV target so we can rebind it through NRD's UAV slot without aliasing.
	RefreshMotionVectors();
	guidesReadyThisFrame = true;
}

void NRD::RefreshMotionVectors() {
    auto motion = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
    auto active=NRDPort::ActiveSize();
    UINT w=static_cast<UINT>(active.x),h=static_cast<UINT>(active.y),x=NRDPort::Eye()*w;
    const D3D11_BOX box{x,0,0,x+w,h,1};
    globals::d3d::context->CopySubresourceRegion(texNRDMV->resource.get(),0,0,0,0,Util::AsReal(motion.texture),0,&box);
}

const nrd::CommonSettings& NRD::GetCommonSettings()
{
	if (commonSettingsValidThisFrame)
		return commonSettings;

	float2 screenSize = NRDPort::FullSize();
	float2 dynres = NRDPort::ActiveSize();

	auto fullW = texNRDViewZ ? texNRDViewZ->desc.Width : (uint32_t)screenSize.x;
	auto fullH = texNRDViewZ ? texNRDViewZ->desc.Height : (uint32_t)screenSize.y;

	commonSettings = nrd::CommonSettings{};
	const uint16_t resourceSize[2] = { (uint16_t)fullW, (uint16_t)fullH };
	const uint16_t rectSize[2] = { (uint16_t)dynres.x, (uint16_t)dynres.y };
	const bool resourceSizeChanged = hasCommonFrameHistory &&
	                                 (resourceSize[0] != prevResourceSize[0] || resourceSize[1] != prevResourceSize[1]);

	commonSettings.resourceSize[0] = resourceSize[0];
	commonSettings.resourceSize[1] = resourceSize[1];
	commonSettings.resourceSizePrev[0] = hasCommonFrameHistory ? prevResourceSize[0] : resourceSize[0];
	commonSettings.resourceSizePrev[1] = hasCommonFrameHistory ? prevResourceSize[1] : resourceSize[1];
	commonSettings.rectSize[0] = rectSize[0];
	commonSettings.rectSize[1] = rectSize[1];
	commonSettings.rectSizePrev[0] = hasCommonFrameHistory ? prevRectSize[0] : rectSize[0];
	commonSettings.rectSizePrev[1] = hasCommonFrameHistory ? prevRectSize[1] : rectSize[1];

	auto viewMat = globals::game::frameBufferCached.GetCameraView(NRDPort::Eye()).Transpose();
	auto projMat = globals::game::frameBufferCached.GetCameraProjUnjittered(NRDPort::Eye()).Transpose();

	auto adjust = globals::game::frameBufferCached.GetCameraPosAdjust(NRDPort::Eye());
	float3 cameraWorldPos{adjust.x,adjust.y,adjust.z};
	DirectX::XMMATRIX translationMat = DirectX::XMMatrixTranslation(-cameraWorldPos.x, -cameraWorldPos.y, -cameraWorldPos.z);
	worldToViewMat = DirectX::XMMatrixMultiply(translationMat, viewMat);

	memcpy(commonSettings.viewToClipMatrix, &projMat, sizeof(float) * 16);
	memcpy(commonSettings.viewToClipMatrixPrev, &prevProjMatrix, sizeof(float) * 16);
	memcpy(commonSettings.worldToViewMatrix, &worldToViewMat, sizeof(float) * 16);
	memcpy(commonSettings.worldToViewMatrixPrev, &prevWorldToViewMat, sizeof(float) * 16);

	commonSettings.motionVectorScale[0] = 1.0f;
	commonSettings.motionVectorScale[1] = 1.0f;
	commonSettings.motionVectorScale[2] = 0.0f;
	commonSettings.isMotionVectorInWorldSpace = false;

	auto jitter = globals::features::upscaling.jitter;
	commonSettings.cameraJitter[0] = jitter.x;
	commonSettings.cameraJitter[1] = jitter.y;
	commonSettings.cameraJitterPrev[0] = prevJitter.x;
	commonSettings.cameraJitterPrev[1] = prevJitter.y;

	const uint32_t gameFrame = globals::state->frameCount;
	commonSettings.frameIndex = gameFrame;
	commonSettings.denoisingRange = 1e6f;

	// A changing rect is normal dynamic-resolution operation; NRD consumes both
	// rectSize and rectSizePrev and should retain history across that transition.
	if (!hasCommonFrameHistory || gameFrame != lastCommonGameFrame + 1 || resourceSizeChanged || globals::state->ShouldResetHistory())
		commonSettings.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;

	const auto frameTime = std::chrono::steady_clock::now();
	if (hasCommonFrameHistory && gameFrame == lastCommonGameFrame + 1) {
		const float frameSeconds = std::chrono::duration<float>(frameTime - lastCommonFrameTime).count();
		if (frameSeconds > 0.0f && frameSeconds < 0.25f)
			smoothedFps += (1.0f / frameSeconds - smoothedFps) * 0.1f;
	}
	lastCommonFrameTime = frameTime;
	lastCommonGameFrame = gameFrame;
	hasCommonFrameHistory = true;

	prevWorldToViewMat = worldToViewMat;
	prevProjMatrix = projMat;
	prevJitter = jitter;
	prevResourceSize[0] = resourceSize[0];
	prevResourceSize[1] = resourceSize[1];
	prevRectSize[0] = rectSize[0];
	prevRectSize[1] = rectSize[1];

	commonSettingsValidThisFrame = true;
	return commonSettings;
}

void NRD::ApplyReblurSettings(nrd::ReblurSettings& out, const REBLURSettings& in, nrd::CheckerboardMode checkerboard) const
{
	const float fps = std::clamp(smoothedFps, 30.0f, 240.0f);
	const auto toFrames = [fps](uint32_t a_framesAt60, uint32_t a_minimum) {
		if (a_framesAt60 == 0)
			return 0u;
		return std::max(nrd::GetMaxAccumulatedFrameNum((float)a_framesAt60 / 60.0f, fps), a_minimum);
	};
	out.maxAccumulatedFrameNum = std::min(toFrames(in.MaxAccumulatedFrameNum, 1), nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
	out.maxFastAccumulatedFrameNum = std::min(toFrames(in.MaxFastAccumulatedFrameNum, 1), out.maxAccumulatedFrameNum);
	out.maxStabilizedFrameNum = std::min(toFrames(in.MaxStabilizedFrameNum, 1), out.maxAccumulatedFrameNum);
	out.historyFixFrameNum = out.maxFastAccumulatedFrameNum > 0 ? std::min(toFrames(in.HistoryFixFrameNum, 1), out.maxFastAccumulatedFrameNum - 1) : 0;
	out.historyFixBasePixelStride = std::max(in.HistoryFixBasePixelStride, 1u);
	out.historyFixAlternatePixelStride = std::max(in.HistoryFixAlternatePixelStride, 1u);
	out.fastHistoryClampingSigmaScale = std::clamp(in.FastHistoryClampingSigmaScale, 1.0f, 3.0f);
	out.diffusePrepassBlurRadius = 0.0f;
	out.minHitDistanceWeight = std::clamp(in.MinHitDistanceWeight, 0.0001f, 0.2f);
	const auto finite = [](float a_value, float a_fallback) { return std::isfinite(a_value) ? a_value : a_fallback; };
	out.minBlurRadius = std::clamp(finite(in.MinBlurRadius, 1.0f), 0.0f, 200.0f);
	out.maxBlurRadius = std::clamp(finite(in.MaxBlurRadius, 30.0f), out.minBlurRadius, 200.0f);
	out.lobeAngleFraction = std::clamp(in.LobeAngleFraction, 0.0f, 1.0f);
	out.roughnessFraction = std::clamp(in.RoughnessFraction, 0.0f, 1.0f);
	out.planeDistanceSensitivity = std::clamp(finite(in.PlaneDistanceSensitivity, 0.02f), 0.0f, 1.0f);
	out.enableAntiFirefly = true;
	out.hitDistanceReconstructionMode = static_cast<nrd::HitDistanceReconstructionMode>(std::min(in.HitDistanceReconstructionMode, 2u));
	out.checkerboardMode = checkerboard;
	out.returnHistoryLengthInsteadOfOcclusion = in.ReturnHistoryLength;
}

bool NRD::DrawReblurSettings(REBLURSettings& s, bool showAdvanced, const char* tag)
{
	bool changed = false;
	ImGui::PushID(tag);

	if (showAdvanced) {
		ImGui::SeparatorText(T(TKEY("accumulation"), "Accumulation"));
		{
			int v = (int)s.MaxAccumulatedFrameNum;
			if (ImGui::SliderInt(T(TKEY("max_accumulated_frames_at_60fps"), "Max Accumulated Frames (at 60 FPS)"), &v, 1, (int)nrd::REBLUR_MAX_HISTORY_FRAME_NUM, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				s.MaxAccumulatedFrameNum = (uint32_t)v;
				changed = true;
			}

			v = (int)s.MaxFastAccumulatedFrameNum;
			if (ImGui::SliderInt(T(TKEY("max_fast_accumulated_frames_at_60fps"), "Max Fast Accumulated Frames (at 60 FPS)"), &v, 1, (int)s.MaxAccumulatedFrameNum, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				s.MaxFastAccumulatedFrameNum = (uint32_t)v;
				changed = true;
			}

			v = (int)s.MaxStabilizedFrameNum;
			if (ImGui::SliderInt(T(TKEY("max_stabilized_frames_at_60fps"), "Max Stabilized Frames (at 60 FPS)"), &v, 0, (int)s.MaxAccumulatedFrameNum, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				s.MaxStabilizedFrameNum = (uint32_t)v;
				changed = true;
			}
		}

		ImGui::SeparatorText(T(TKEY("spatial_filter"), "Spatial Filter"));
		{
			changed |= ImGui::SliderFloat(T(TKEY("min_blur_radius"), "Min Blur Radius"), &s.MinBlurRadius, 0.0f, 10.0f, "%.1f px");
			changed |= ImGui::SliderFloat(T(TKEY("max_blur_radius"), "Max Blur Radius"), &s.MaxBlurRadius, 0.0f, 60.0f, "%.1f px");
			changed |= ImGui::SliderFloat(T(TKEY("lobe_angle_fraction"), "Lobe Angle Fraction"), &s.LobeAngleFraction, 0.0f, 1.0f, "%.2f");
			changed |= ImGui::SliderFloat(T(TKEY("roughness_fraction"), "Roughness Fraction"), &s.RoughnessFraction, 0.0f, 1.0f, "%.2f");
			changed |= ImGui::SliderFloat(T(TKEY("plane_distance_sensitivity"), "Plane Distance Sensitivity"), &s.PlaneDistanceSensitivity, 0.0f, 0.1f, "%.4f");
		}

		ImGui::SeparatorText(T(TKEY("quality"), "Quality"));
		{
			changed |= ImGui::SliderFloat(T(TKEY("fast_history_clamping_sigma"), "Fast History Clamping Sigma"), &s.FastHistoryClampingSigmaScale, 1.0f, 3.0f, "%.2f");
			changed |= ImGui::SliderFloat(T(TKEY("min_hit_distance_weight"), "Min Hit Distance Weight"), &s.MinHitDistanceWeight, 0.0001f, 0.2f, "%.4f");

			int v = (int)s.HistoryFixFrameNum;
			if (ImGui::SliderInt(T(TKEY("history_fix_frames_at_60fps"), "History Fix Frames (at 60 FPS)"), &v, 0, 4, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				s.HistoryFixFrameNum = (uint32_t)v;
				changed = true;
			}

			v = (int)s.HistoryFixBasePixelStride;
			if (ImGui::SliderInt(T(TKEY("history_fix_pixel_stride"), "History Fix Pixel Stride"), &v, 1, 20, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				s.HistoryFixBasePixelStride = (uint32_t)v;
				changed = true;
			}
		}

		ImGui::SeparatorText(T(TKEY("debug"), "Debug"));
		{
			changed |= ImGui::SliderFloat(T(TKEY("split_screen"), "Split Screen"), &s.SplitScreen, 0.0f, 1.0f, "%.2f");
			changed |= ImGui::Checkbox(T(TKEY("nrd_validation_overlay"), "NRD Validation Overlay"), &s.EnableValidation);
			changed |= ImGui::Checkbox(T(TKEY("output_history_length"), "Output History Length"), &s.ReturnHistoryLength);

			const char* hitDistReconModes[] = { T(TKEY("hit_distance_off"), "Off"), T(TKEY("hit_distance_area_3x3"), "3x3 Area"), T(TKEY("hit_distance_area_5x5"), "5x5 Area") };
			int hdMode = (int)s.HitDistanceReconstructionMode;
			if (ImGui::Combo(T(TKEY("hit_distance_reconstruction"), "Hit Distance Reconstruction"), &hdMode, hitDistReconModes, 3)) {
				s.HitDistanceReconstructionMode = (uint32_t)hdMode;
				changed = true;
			}
		}
	}

	ImGui::PopID();
	return changed;
}

#undef I18N_KEY_PREFIX
