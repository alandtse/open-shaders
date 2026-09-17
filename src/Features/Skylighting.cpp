#include "Skylighting.h"

#include "Deferred.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/MathUtils.h"

#include <cmath>
#include <memory>
#include <numbers>

#define I18N_KEY_PREFIX "feature.skylighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skylighting::Settings,
	MaxZenith,
	MinDiffuseVisibility,
	MinSpecularVisibility,
	ProbeGridQuality,
	EnableIncrementalProbeUpdates,
	StableSliceCount,
	EnableReducedUpdateFrequency,
	OcclusionUpdateInterval,
	ProbeUpdateInterval,
	ProbeArrayWorldSizeCells,
	EnableFastProbeSampling)

void Skylighting::LoadSettings(json& o_json)
{
	const auto previous = settings;
	settings = o_json;
	settings.OcclusionUpdateInterval = std::clamp(settings.OcclusionUpdateInterval, 1u, 32u);
	settings.ProbeUpdateInterval = std::clamp(settings.ProbeUpdateInterval, settings.OcclusionUpdateInterval, 32u);
	if (previous.EnableReducedUpdateFrequency != settings.EnableReducedUpdateFrequency || previous.OcclusionUpdateInterval != settings.OcclusionUpdateInterval || previous.ProbeUpdateInterval != settings.ProbeUpdateInterval)
		ResetSkylighting();
	settings.StableSliceCount = std::clamp(settings.StableSliceCount, 1u, 128u);
	if (previous.EnableIncrementalProbeUpdates != settings.EnableIncrementalProbeUpdates || previous.StableSliceCount != settings.StableSliceCount)
		ResetSkylighting();
	settings.ProbeGridQuality = std::min(settings.ProbeGridQuality, 2u);
	settings.ProbeArrayWorldSizeCells = Util::ClampFinite(settings.ProbeArrayWorldSizeCells, Settings::kMinProbeFieldSizeCells, Settings::kMaxProbeFieldSizeCells, Settings{}.ProbeArrayWorldSizeCells);
}

void Skylighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skylighting::RestoreDefaultSettings()
{
	settings = {};
	ResetSkylighting();
}

void Skylighting::ResetSkylighting()
{
	queuedResetSkylighting.store(true);
}

bool Skylighting::HasProbeResources() const
{
	return texOcclusion && texOcclusion->srv && texOcclusion->dsv &&
	       texProbeArray && texProbeArray->srv && texProbeArray->uav &&
	       texAccumFramesArray && texAccumFramesArray->uav &&
	       texShadowBitmask && texShadowBitmask->uav &&
	       texShadowVisibility && texShadowVisibility->srv && texShadowVisibility->uav;
}

void Skylighting::ClearProbes()
{
	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* nullProbe = nullptr;
	context->PSSetShaderResources(50, 1, &nullProbe);
	context->PSSetShaderResources(53, 1, &nullProbe);

	const float unitSH[4] = { std::sqrt(4.0f * std::numbers::pi_v<float>), 0.0f, 0.0f, 0.0f };
	context->ClearUnorderedAccessViewFloat(texProbeArray->uav.get(), unitSH);

	UINT clr[1] = { 0 };
	context->ClearUnorderedAccessViewUint(texAccumFramesArray->uav.get(), clr);
	context->ClearUnorderedAccessViewUint(texShadowBitmask->uav.get(), clr);

	float clrf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	context->ClearUnorderedAccessViewFloat(texShadowVisibility->uav.get(), clrf);

	probeDataReady = false;
	sliceCursor = 0;
	sliceCaptureMask = 0;
	forcedFullUpdateFrames = 4;
	lastProbeUpdateCapture = static_cast<uint>(-1);
	lastProbeUpdateFrame = static_cast<uint>(-1);
	occlusionCaptureCorner = 0;
	nextOcclusionCorner = 0;
	lastOcclusionRenderFrame = static_cast<uint>(-1);
}

void Skylighting::DrawSettings()
{
	ImGui::Text("%s", T(TKEY("min_visibility_desc"), "Minimum visibility values. Diffuse darkens objects. Specular removes the sky from reflections."));
	ImGui::SliderFloat(T(TKEY("diffuse_min_visibility"), "Diffuse Min Visibility"), &settings.MinDiffuseVisibility, 0.01f, 1.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("specular_min_visibility"), "Specular Min Visibility"), &settings.MinSpecularVisibility, 0.01f, 1.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("probe_field_width"), "Probe Field Width (Cells)"), &settings.ProbeArrayWorldSizeCells, Settings::kMinProbeFieldSizeCells, Settings::kMaxProbeFieldSizeCells, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("probe_field_width_desc"), "Extends skylighting coverage without adding probes. Larger fields reduce spatial detail and rebuild the probe history."));

	const char* gridNames[] = {
		T(TKEY("probe_grid_low"), "128 x 128 x 64"),
		T(TKEY("probe_grid_medium"), "192 x 192 x 96"),
		T(TKEY("probe_grid_high"), "256 x 256 x 128")
	};
	int selectedGrid = static_cast<int>(settings.ProbeGridQuality);
	if (ImGui::Combo(T(TKEY("probe_grid"), "Probe Grid"), &selectedGrid, gridNames, 3))
		settings.ProbeGridQuality = static_cast<uint>(selectedGrid);

	if (ImGui::Checkbox(T(TKEY("incremental_updates"), "Incremental Probe Updates"), &settings.EnableIncrementalProbeUpdates))
		ResetSkylighting();
	int sliceCount = static_cast<int>(settings.StableSliceCount);
	if (ImGui::SliderInt(T(TKEY("slice_count"), "Probe Slices per Update"), &sliceCount, 1, 128, "%d", ImGuiSliderFlags_AlwaysClamp)) {
		settings.StableSliceCount = static_cast<uint>(sliceCount);
		ResetSkylighting();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("incremental_tooltip"), "Updates a smaller depth range while stationary. Lower counts reduce update work but take longer to refresh the whole field. Movement and rebuilds update the full grid."));

	if (ImGui::Checkbox(T(TKEY("reduced_frequency"), "Reduced Update Frequency"), &settings.EnableReducedUpdateFrequency))
		ResetSkylighting();
	int captureInterval = static_cast<int>(settings.OcclusionUpdateInterval);
	int probeInterval = static_cast<int>(settings.ProbeUpdateInterval);
	bool cadenceChanged = ImGui::SliderInt(T(TKEY("capture_interval"), "Occlusion Capture Interval"), &captureInterval, 1, 32, "%d frames", ImGuiSliderFlags_AlwaysClamp);
	cadenceChanged |= ImGui::SliderInt(T(TKEY("probe_interval"), "Full-grid Probe Interval"), &probeInterval, captureInterval, 32, "%d frames", ImGuiSliderFlags_AlwaysClamp);
	if (cadenceChanged) {
		settings.OcclusionUpdateInterval = static_cast<uint>(captureInterval);
		settings.ProbeUpdateInterval = static_cast<uint>(std::max(captureInterval, probeInterval));
		ResetSkylighting();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("cadence_tooltip"), "Intervals are minimum frame delays while stationary. Probe updates consume fresh captures; incremental updates consume every captured quadrant. Movement and rebuilds bypass the delays."));
	ImGui::Checkbox(T(TKEY("fast_probe_sampling"), "Fast Probe Sampling"), &settings.EnableFastProbeSampling);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("fast_probe_sampling_desc"), "Skips normal-based probe weighting. May increase light leaking near thin geometry."));

	ImGui::Separator();

	if (ImGui::Button(T(TKEY("rebuild"), "Rebuild Skylighting")))
		ResetSkylighting();

	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("rebuild_tooltip"), "Changes below require rebuilding, a loading screen, or moving away from the current location to apply."));

	ImGui::SliderAngle(T(TKEY("max_zenith"), "Max Zenith Angle"), &settings.MaxZenith, 0, 90);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("max_zenith_tooltip"), "Smaller angles creates more focused top-down shadow."));
}

void Skylighting::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(Util::AsW32(&texDesc));
		precipitationOcclusion.depthSRV->GetDesc(Util::AsW32(&srvDesc));
		precipitationOcclusion.views[0]->GetDesc(Util::AsW32(&dsvDesc));

		texOcclusion = new Texture2D(texDesc, "Skylighting::Occlusion");
		texOcclusion->CreateSRV(srvDesc);
		texOcclusion->CreateDSV(dsvDesc);
	}

	CreateProbeResources(GetProbeArrayDims(settings.ProbeGridQuality));
	activeProbeGridQuality = settings.ProbeGridQuality;

	ClearProbes();

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;  // Use comparison filtering
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;               // Address mode (Clamp for shadow maps)
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;  // Comparison function
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, comparisonSampler.put()));
		Util::SetResourceName(comparisonSampler.get(), "Skylighting::ComparisonSampler");
	}

	CompileComputeShaders();
}

std::array<uint, 3> Skylighting::GetProbeArrayDims(uint quality)
{
	static constexpr std::array<std::array<uint, 3>, 3> dimensions{ { { 128, 128, 64 }, { 192, 192, 96 }, { 256, 256, 128 } } };
	return dimensions[std::min(quality, 2u)];
}

void Skylighting::CreateProbeResources(const std::array<uint, 3>& dimensions)
{
	{
		D3D11_TEXTURE3D_DESC texDesc{
			.Width = dimensions[0],
			.Height = dimensions[1],
			.Depth = dimensions[2],
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MipSlice = 0,
				.FirstWSlice = 0,
				.WSize = texDesc.Depth }
		};

		auto newProbeArray = std::make_unique<Texture3D>(texDesc, "Skylighting::ProbeArray");
		newProbeArray->CreateSRV(srvDesc);
		newProbeArray->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_UINT;

		auto newAccumFramesArray = std::make_unique<Texture3D>(texDesc, "Skylighting::AccumFramesArray");
		newAccumFramesArray->CreateSRV(srvDesc);
		newAccumFramesArray->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_UINT;

		auto newShadowBitmask = std::make_unique<Texture3D>(texDesc, "Skylighting::ShadowBitmask");
		newShadowBitmask->CreateSRV(srvDesc);
		newShadowBitmask->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UNORM;

		auto newShadowVisibility = std::make_unique<Texture3D>(texDesc, "Skylighting::ShadowVisibility");
		newShadowVisibility->CreateSRV(srvDesc);
		newShadowVisibility->CreateUAV(uavDesc);

		delete texProbeArray;
		texProbeArray = newProbeArray.release();
		delete texAccumFramesArray;
		texAccumFramesArray = newAccumFramesArray.release();
		delete texShadowBitmask;
		texShadowBitmask = newShadowBitmask.release();
		delete texShadowVisibility;
		texShadowVisibility = newShadowVisibility.release();
		std::copy(dimensions.begin(), dimensions.end(), probeArrayDims);
	}
}

void Skylighting::ApplyProbeGrid()
{
	if (!texProbeArray || settings.ProbeGridQuality == activeProbeGridQuality)
		return;

	try {
		CreateProbeResources(GetProbeArrayDims(settings.ProbeGridQuality));
		activeProbeGridQuality = settings.ProbeGridQuality;
		ResetSkylighting();
	} catch (const std::exception& error) {
		logger::error("Skylighting probe grid allocation failed; retaining the active grid: {}", error.what());
		settings.ProbeGridQuality = activeProbeGridQuality;
	}
}

void Skylighting::ClearShaderCache()
{
	Util::ClearShaders<ID3D11ComputeShader>({ probeUpdateCompute, occlusionOnlyProbeUpdateCompute });

	CompileComputeShaders();
	ResetSkylighting();
}

void Skylighting::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		const char* resourceName;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &probeUpdateCompute, "UpdateProbesCS.hlsl", {}, "Skylighting::ProbeUpdateCS" },
			{ &occlusionOnlyProbeUpdateCompute, "UpdateProbesCS.hlsl", { { "OCCLUSION_ONLY", "" } }, "Skylighting::OcclusionOnlyProbeUpdateCS" },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\Skylighting") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0"))) {
			info.programPtr->attach(rawPtr);
			Util::SetResourceName(rawPtr, info.resourceName);
		}
	}
}

float3 Skylighting::GetProbeCellSize() const
{
	return { occlusionDistance / probeArrayDims[0], occlusionDistance / probeArrayDims[1], occlusionDistance * .5f / probeArrayDims[2] };
}

float3 Skylighting::GetProbeCell(float3 eyePosition) const
{
	const auto cellID = eyePosition / GetProbeCellSize();
	return { round(cellID.x), round(cellID.y), round(cellID.z) };
}

Skylighting::SkylightingCB Skylighting::GetCommonBufferData(bool a_inWorld)
{
	ApplyProbeGrid();

	if (!a_inWorld)
		return Skylighting::SkylightingCB{};

	if (globals::state->isMapMenuOpen)
		return Skylighting::SkylightingCB{};

	auto eyePosNI = Util::GetEyePosition(0);
	auto eyePos = float3{ eyePosNI.x, eyePosNI.y, eyePosNI.z };

	const auto cellSize = GetProbeCellSize();
	const auto cellID = GetProbeCell(eyePos);
	auto cellOrigin = cellID * cellSize;
	float3 cellIDDiff = previousProbeCell - cellID;
	pendingProbeCell = cellID;
	if (cellIDDiff.x != 0 || cellIDDiff.y != 0 || cellIDDiff.z != 0) {
		forcedFullUpdateFrames = 4;
		sliceCursor = 0;
		sliceCaptureMask = 0;
	}
	dispatchSliceStart = 0;
	dispatchSliceCount = probeArrayDims[2];
	if (settings.EnableIncrementalProbeUpdates && forcedFullUpdateFrames == 0) {
		dispatchSliceStart = sliceCursor;
		dispatchSliceCount = std::min(std::clamp(settings.StableSliceCount, 1u, probeArrayDims[2]), probeArrayDims[2] - sliceCursor);
	}

	return {
		.OcclusionViewProj = OcclusionTransform,
		.OcclusionDir = OcclusionDir,
		.PosOffset = cellOrigin - eyePos,
		.ArrayOrigin = {
			static_cast<uint>((static_cast<int>(cellID.x) - static_cast<int>(probeArrayDims[0] / 2)) % static_cast<int>(probeArrayDims[0]) + static_cast<int>(probeArrayDims[0])) % probeArrayDims[0],
			static_cast<uint>((static_cast<int>(cellID.y) - static_cast<int>(probeArrayDims[1] / 2)) % static_cast<int>(probeArrayDims[1]) + static_cast<int>(probeArrayDims[1])) % probeArrayDims[1],
			static_cast<uint>((static_cast<int>(cellID.z) - static_cast<int>(probeArrayDims[2] / 2)) % static_cast<int>(probeArrayDims[2]) + static_cast<int>(probeArrayDims[2])) % probeArrayDims[2] },
		.ValidMargin = { (int)cellIDDiff.x, (int)cellIDDiff.y, (int)cellIDDiff.z },
		.MinDiffuseVisibility = settings.MinDiffuseVisibility,
		.MinSpecularVisibility = settings.MinSpecularVisibility,
		.ProbeDataReady = probeDataReady && HasProbeResources() && !queuedResetSkylighting.load(),
		.ProbeArrayWorldSize = occlusionDistance,
		.ArrayDims = { probeArrayDims[0], probeArrayDims[1], probeArrayDims[2] },
		.SliceStart = dispatchSliceStart,
		.SliceCount = dispatchSliceCount,
		.EnableFastProbeSampling = settings.EnableFastProbeSampling
	};
}

void Skylighting::Prepass()
{
	if (globals::state->isMapMenuOpen || !HasProbeResources())
		return;

	auto context = globals::d3d::context;
	const bool interior = Util::IsInterior();
	if (queuedResetSkylighting.exchange(false))
		ClearProbes();

	if (!previousInteriorState || *previousInteriorState != interior) {
		ClearProbes();
		previousInteriorState = interior;
	}

	if (interior)
		RenderOcclusion();

	globals::state->UpdateFeatureData(true);

	auto* updateShader = interior ? occlusionOnlyProbeUpdateCompute.get() : probeUpdateCompute.get();
	if (!updateShader || !comparisonSampler) {
		probeDataReady = false;
		globals::state->UpdateFeatureData(true);
	}
	const uint probeInterval = std::max(settings.OcclusionUpdateInterval, settings.ProbeUpdateInterval);
	const bool probeUpdateDue = !settings.EnableReducedUpdateFrequency || forcedFullUpdateFrames > 0 ||
	                            settings.EnableIncrementalProbeUpdates || globals::state->frameCount - lastProbeUpdateFrame >= probeInterval;
	if (updateShader && comparisonSampler && probeUpdateDue && lastOcclusionRenderFrame == globals::state->frameCount && lastProbeUpdateCapture != frameCount) {
		CS_GPU_PASS_SELECT(interior, "Skylighting::InteriorProbeUpdate", "Skylighting::ProbeUpdate");

		auto renderer = globals::game::renderer;
		auto& cascadeDepthStencil = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kSHADOWMAPS_ESRAM];

		std::array<ID3D11ShaderResourceView*, 4> srvs = {
			texOcclusion->srv.get(),
			nullptr,
			interior ? nullptr : globals::deferred->directionalShadowLights->srv.get(),
			interior ? nullptr : Util::AsReal(cascadeDepthStencil.depthSRV)
		};
		std::array<ID3D11UnorderedAccessView*, 4> uavs = {
			texProbeArray->uav.get(),
			texAccumFramesArray->uav.get(),
			interior ? nullptr : texShadowBitmask->uav.get(),
			interior ? nullptr : texShadowVisibility->uav.get()
		};
		std::array<ID3D11SamplerState*, 1> samplers = {
			comparisonSampler.get()
		};

		// Update probe array
		{
			ID3D11ShaderResourceView* nullProbe = nullptr;
			context->PSSetShaderResources(50, 1, &nullProbe);
			context->PSSetShaderResources(53, 1, &nullProbe);
			context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(updateShader, nullptr, 0);
			context->Dispatch((probeArrayDims[0] + 7u) >> 3, (probeArrayDims[1] + 7u) >> 3, dispatchSliceCount);
			lastProbeUpdateCapture = frameCount;
			lastProbeUpdateFrame = globals::state->frameCount;
			if (forcedFullUpdateFrames > 0) {
				--forcedFullUpdateFrames;
			} else if (settings.EnableIncrementalProbeUpdates) {
				sliceCaptureMask |= 1u << occlusionCaptureCorner;
				if (sliceCaptureMask == 0xFu) {
					sliceCursor = (dispatchSliceStart + dispatchSliceCount) % probeArrayDims[2];
					sliceCaptureMask = 0;
				}
			}
			nextOcclusionCorner = (occlusionCaptureCorner + 1u) % 4;
			previousProbeCell = pendingProbeCell;
			if (!probeDataReady) {
				probeDataReady = true;
				globals::state->UpdateFeatureData(true);
			}
		}

		// Reset
		{
			srvs.fill(nullptr);
			uavs.fill(nullptr);
			samplers.fill(nullptr);

			context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}
	}

	// Set PS shader resources
	{
		ID3D11ShaderResourceView* srv = probeDataReady ? texProbeArray->srv.get() : nullptr;
		context->PSSetShaderResources(50, 1, &srv);

		srv = !probeDataReady || interior ? nullptr : texShadowVisibility->srv.get();
		context->PSSetShaderResources(53, 1, &srv);
	}
}

void Skylighting::PostPostLoad()
{
	logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
	stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
	stl::write_vfunc<0x6, BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
	stl::write_vfunc<0x7, BSUtilityShader_RestoreGeometry>(RE::VTABLE_BSUtilityShader[0]);
	stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate<std::uintptr_t>(0x3A1, REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0x3BF : 0x3A1, 0x2FA));

	if (globals::game::isVR)
		stl::write_thunk_call<SetViewFrustumVR>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));
	else
		stl::write_thunk_call<SetViewFrustum>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));
}

void Skylighting::GameLoaded()
{
	ResetSkylighting();
}

void Skylighting::OnSceneTransitionReset(bool)
{
	ResetSkylighting();
}

void Skylighting::RegisterUxActions()
{
	FEATURE_COMMAND("rebuild", "Queue a Skylighting probe rebuild on the render thread.",
		[](Feature* self, const json&) { static_cast<Skylighting*>(self)->ResetSkylighting(); });
}

//////////////////////////////////////////////////////////////

struct BSParticleShaderRainEmitter
{
	void* vftable_BSParticleShaderRainEmitter_0;
	char _pad_8[4056];
};

enum class ShaderTechnique
{
	// Sky
	SkySunOcclude = 0x2,

	// Grass
	GrassNoAlphaDirOnlyFlatLit = 0x3,
	GrassNoAlphaDirOnlyFlatLitSlope = 0x5,
	GrassNoAlphaDirOnlyVertLitSlope = 0x6,
	GrassNoAlphaDirOnlyFlatLitBillboard = 0x13,
	GrassNoAlphaDirOnlyFlatLitSlopeBillboard = 0x14,

	// Utility
	UtilityGeneralStart = 0x2B,

	// Effect
	EffectGeneralStart = 0x4000002C,

	// Lighting
	LightingGeneralStart = 0x4800002D,

	// DistantTree
	DistantTreeDistantTreeBlock = 0x5C00002E,
	DistantTreeDepth = 0x5C00002F,

	// Grass
	GrassDirOnlyFlatLit = 0x5C000030,
	GrassDirOnlyFlatLitSlope = 0x5C000032,
	GrassDirOnlyVertLitSlope = 0x5C000033,
	GrassDirOnlyFlatLitBillboard = 0x5C000040,
	GrassDirOnlyFlatLitSlopeBillboard = 0x5C000041,
	GrassRenderDepth = 0x5C00005C,

	// Sky
	SkySky = 0x5C00005E,
	SkyMoonAndStarsMask = 0x5C00005F,
	SkyStars = 0x5C000060,
	SkyTexture = 0x5C000061,
	SkyClouds = 0x5C000062,
	SkyCloudsLerp = 0x5C000063,
	SkyCloudsFade = 0x5C000064,

	// Particle
	ParticleParticles = 0x5C000065,
	ParticleParticlesGryColorAlpha = 0x5C000066,
	ParticleParticlesGryColor = 0x5C000067,
	ParticleParticlesGryAlpha = 0x5C000068,
	ParticleEnvCubeSnow = 0x5C000069,
	ParticleEnvCubeRain = 0x5C00006A,

	// Water
	WaterSimple = 0x5C00006B,
	WaterSimpleVc = 0x5C00006C,
	WaterStencil = 0x5C00006D,
	WaterStencilVc = 0x5C00006E,
	WaterDisplacementStencil = 0x5C00006F,
	WaterDisplacementStencilVc = 0x5C000070,
	WaterGeneralStart = 0x5C000071,

	// Sky
	SkySunGlare = 0x5C006072,

	// BloodSplater
	BloodSplaterFlare = 0x5C006073,
	BloodSplaterSplatter = 0x5C006074,
};

//////////////////////////////////////////////////////////////

RE::BSShaderProperty::RenderPassArray* Skylighting::BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl::thunk(
	RE::BSLightingShaderProperty* property,
	RE::BSGeometry* geometry,
	[[maybe_unused]] uint32_t renderMode,
	[[maybe_unused]] RE::BSGraphics::BSShaderAccumulator* accumulator)
{
	auto& skylighting = globals::features::skylighting;

	auto batch = accumulator->GetRuntimeData().batchRenderer;
	batch->geometryGroups[14]->flags &= ~1;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	using enum RE::BSUtilityShader::Flags;

	auto* precipitationOcclusionMapRenderPassList = &property->occlusionPasses;

	precipitationOcclusionMapRenderPassList->Clear();
	if (skylighting.inOcclusion) {
		if (property->flags.any(kSkinned) && property->flags.none(kTreeAnim))
			return precipitationOcclusionMapRenderPassList;
	} else {
		if (property->flags.any(kSkinned))
			return precipitationOcclusionMapRenderPassList;
	}

	const bool validOccluder = property->flags.any(kZBufferWrite) &&
	                           property->flags.none(kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal) &&
	                           (skylighting.inOcclusion || property->flags.none(kMultiTextureLandscape, kNoLODLandBlend));
	if (!validOccluder || !(geometry->worldBound.radius > 32))
		return precipitationOcclusionMapRenderPassList;

	if (skylighting.inOcclusion) {
		if (geometry->GetUserData()) {
			RE::BSFadeNode* fadeNode = nullptr;

			RE::NiNode* parent = geometry->parent;
			while (parent && !fadeNode) {
				fadeNode = parent->AsFadeNode();
				parent = parent->parent;
			}

			if (fadeNode) {
				if (auto extraData = fadeNode->GetExtraData("BSX")) {
					auto bsxFlags = (RE::BSXFlags*)extraData;
					auto value = static_cast<int32_t>(bsxFlags->value);

					if (value & (static_cast<int32_t>(RE::BSXFlags::Flag::kRagdoll) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kEditorMarker) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kDynamic) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kAddon) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kNeedsTransformUpdate) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kMagicShaderParticles) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kLights) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kBreakable) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kSearchedBreakable))) {
						return precipitationOcclusionMapRenderPassList;
					}
				}
			}
		}
	}

	stl::enumeration<RE::BSUtilityShader::Flags> technique;
	technique.set(RenderDepth);

	if (property->flags.any(kVertexColors)) {
		technique.set(Vc);
	}

	const auto alphaProperty = static_cast<RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().alphaProperty.get());
	if (alphaProperty && alphaProperty->GetAlphaTesting()) {
		technique.set(Texture);
		technique.set(AlphaTest);
	}

	if (property->flags.any(kLODObjects, kHDLODObjects)) {
		technique.set(LodObject);
	}

	if (property->flags.any(kTreeAnim)) {
		technique.set(TreeAnim);
	}

	precipitationOcclusionMapRenderPassList->EmplacePass(
		globals::game::utilityShader,
		property,
		geometry,
		technique.underlying() + static_cast<uint32_t>(ShaderTechnique::UtilityGeneralStart));

	return precipitationOcclusionMapRenderPassList;
}

void Skylighting::SetViewFrustum::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		uint corner = skylighting.occlusionCaptureCorner;

		float frustumSize = a_frustum->fTop;

		a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
		a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
		a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
		a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
	}

	func(a_camera, a_frustum);
}

void Skylighting::SetViewFrustumVR::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum, uint a_eyeIndex)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		uint corner = skylighting.occlusionCaptureCorner;

		float frustumSize = a_frustum->fTop;

		a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
		a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
		a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
		a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
	}

	func(a_camera, a_frustum, a_eyeIndex);
}

void Skylighting::RenderOcclusion()
{
	ZoneScopedS(8);
	auto shaderCache = globals::shaderCache;
	auto renderer = globals::game::renderer;
	auto sky = globals::game::sky;
	const bool interior = Util::IsInterior();

	if (!shaderCache->IsEnabled()) {
		if (!interior) {
			CS_GPU_PASS("Skylighting::PrecipitationMask");
			Main_Precipitation_RenderOcclusion::func();
		}
		return;
	}

	if (!renderer || !sky || !sky->precip)
		return;

	auto* precipitation = sky->precip;
	if (!interior) {
		CS_GPU_PASS("Skylighting::PrecipitationMask");
		auto precipitationObject = precipitation->currentPrecip ? precipitation->currentPrecip : precipitation->lastPrecip;
		if (precipitationObject) {
			auto* particleProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(
				precipitationObject->GetGeometryRuntimeData().shaderProperty.get());
			if (particleProperty && particleProperty->particleEmitter) {
				precipitation->SetupMask();
				precipitation->RenderMask(static_cast<RE::BSParticleShaderRainEmitter*>(particleProperty->particleEmitter));
			}
		}
	}

	if (!HasProbeResources())
		return;

	if (queuedResetSkylighting.exchange(false))
		ClearProbes();

	if (lastOcclusionRenderFrame == globals::state->frameCount)
		return;

	const float requestedDistance = settings.ProbeArrayWorldSizeCells * Settings::kWorldCellSize;
	if (requestedDistance != occlusionDistance) {
		occlusionDistance = requestedDistance;
		ClearProbes();
	}

	const auto eyePosition = Util::GetEyePosition(0);
	const auto cellID = GetProbeCell({ eyePosition.x, eyePosition.y, eyePosition.z });
	const bool cellMoved = cellID.x != previousProbeCell.x || cellID.y != previousProbeCell.y || cellID.z != previousProbeCell.z;
	if (settings.EnableReducedUpdateFrequency && forcedFullUpdateFrames == 0 && !cellMoved &&
		globals::state->frameCount - lastOcclusionRenderFrame < settings.OcclusionUpdateInterval)
		return;

	CS_GPU_PASS("Skylighting::SkylightingMask");
	occlusionCaptureCorner = nextOcclusionCorner;
	++frameCount;

	auto& precipitationTarget = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
	const RE::BSGraphics::DepthStencilData originalTarget = precipitationTarget;
	static float& precipitationCubeSize = *reinterpret_cast<float*>(REL::RelocationID(515451, 401590).address());
	static RE::NiPoint3& precipitationDirection = *reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(515509, 401648).address());
	static REL::Relocation<void(RE::Precipitation*, RE::NiPointer<RE::NiCamera>)> computeProjection{ REL::RelocationID(25643, 26185) };
	const float originalCubeSize = precipitationCubeSize;
	const float originalLastCubeSize = precipitation->lastCubeSize;
	const RE::NiPoint3 originalDirection = precipitationDirection;
	const bool originalOcclusionState = inOcclusion;
	bool projectionChanged = false;

	const SKSE::stl::scope_exit restoreEngineState([&]() noexcept {
		while (rasterCullOverrideDepth > 0)
			EndInteriorOcclusionGeometry();
		forceInteriorOcclusionTwoSided = false;
		inOcclusion = originalOcclusionState;
		precipitationCubeSize = originalCubeSize;
		precipitation->lastCubeSize = originalLastCubeSize;
		precipitationDirection = originalDirection;
		precipitationTarget = originalTarget;
		if (projectionChanged) {
			ZoneScopedN("Skylighting - Restore Projection");
			computeProjection(precipitation, precipitation->occlusionData.camera);
		}
	});

	precipitationTarget.depthSRV = Util::AsW32(texOcclusion->srv.get());
	precipitationTarget.texture = Util::AsW32(texOcclusion->resource.get());
	precipitationTarget.views[0] = Util::AsW32(texOcclusion->dsv.get());
	inOcclusion = true;
	forceInteriorOcclusionTwoSided = interior;
	precipitationCubeSize = occlusionDistance;
	precipitation->lastCubeSize = precipitationCubeSize;

	constexpr float reciprocalRandMax = 1.0f / RAND_MAX;
	static int randomSeed = std::rand();
	static uint randomFrame = 0;
	float2 diskPoint = float2(randomSeed * reciprocalRandMax) +
	                   static_cast<float>(randomFrame) * float2(0.245122333753f, 0.430159709002f);
	diskPoint.x -= std::floor(diskPoint.x);
	diskPoint.y -= std::floor(diskPoint.y);
	if (++randomFrame == 1000) {
		randomFrame = 0;
		randomSeed = std::rand();
	}
	diskPoint.x = std::sqrt(diskPoint.x * std::sin(settings.MaxZenith));
	diskPoint.y *= 2.0f * std::numbers::pi_v<float>;
	diskPoint = float2{ diskPoint.x * std::cos(diskPoint.y), diskPoint.x * std::sin(diskPoint.y) };

	float3 direction = -float3{ diskPoint.x, diskPoint.y, std::sqrt(std::max(0.0f, 1.0f - diskPoint.LengthSquared())) };
	direction.Normalize();
	precipitationDirection = { direction.x, direction.y, direction.z };

	{
		ZoneScopedN("Skylighting - Setup Projection");
		computeProjection(precipitation, precipitation->occlusionData.camera);
		projectionChanged = true;
		precipitation->SetupMask();
	}

	BSParticleShaderRainEmitter syntheticRain{};
	{
		CS_GPU_PASS("Skylighting::OcclusionMask");
		precipitation->RenderMask(reinterpret_cast<RE::BSParticleShaderRainEmitter*>(&syntheticRain));
	}

	OcclusionDir = -float4{ direction.x, direction.y, direction.z, 0.0f };
	OcclusionTransform = reinterpret_cast<RE::BSParticleShaderRainEmitter*>(&syntheticRain)->occlusionProjection;
	lastOcclusionRenderFrame = globals::state->frameCount;
}

void Skylighting::Main_Precipitation_RenderOcclusion::thunk()
{
	globals::features::skylighting.RenderOcclusion();
}

void Skylighting::BSUtilityShader_SetupGeometry::thunk(
	RE::BSShader* a_shader,
	RE::BSRenderPass* a_pass,
	uint32_t a_renderFlags)
{
	func(a_shader, a_pass, a_renderFlags);
	globals::features::skylighting.BeginInteriorOcclusionGeometry();
}

void Skylighting::BSUtilityShader_RestoreGeometry::thunk(
	RE::BSShader* a_shader,
	RE::BSRenderPass* a_pass,
	uint32_t a_renderFlags)
{
	func(a_shader, a_pass, a_renderFlags);
	globals::features::skylighting.EndInteriorOcclusionGeometry();
}

uint32_t* Skylighting::GetRasterCullMode() const
{
	auto* shadowState = globals::game::shadowState;
	if (!shadowState)
		return nullptr;

	return globals::game::isVR ?
	           &shadowState->GetVRRuntimeData().rasterStateCullMode :
	           &shadowState->GetRuntimeData().rasterStateCullMode;
}

void Skylighting::BeginInteriorOcclusionGeometry()
{
	if (!forceInteriorOcclusionTwoSided)
		return;

	auto* rasterCullMode = GetRasterCullMode();
	if (!rasterCullMode)
		return;

	if (rasterCullOverrideDepth++ == 0)
		savedRasterCullMode = *rasterCullMode;

	constexpr uint32_t cullModeNone = 0;
	if (*rasterCullMode != cullModeNone) {
		*rasterCullMode = cullModeNone;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);
	}
}

void Skylighting::EndInteriorOcclusionGeometry()
{
	if (rasterCullOverrideDepth == 0)
		return;
	if (--rasterCullOverrideDepth != 0)
		return;

	if (auto* rasterCullMode = GetRasterCullMode(); rasterCullMode && *rasterCullMode != savedRasterCullMode) {
		*rasterCullMode = savedRasterCullMode;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);
	}
}

#undef I18N_KEY_PREFIX
