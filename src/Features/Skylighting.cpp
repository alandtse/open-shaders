#include "Skylighting.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "GpuPass.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.skylighting."

namespace
{
	constexpr uint kOcclusionCornerCount = 4;
	constexpr uint kAllOcclusionCornersMask = (1u << kOcclusionCornerCount) - 1u;

	struct ProbeGridPreset
	{
		uint Width;
		uint Height;
		uint Depth;
	};

	constexpr std::array<ProbeGridPreset, 5> kProbeGridPresets = {
		ProbeGridPreset{ 128, 128, 64 },
		ProbeGridPreset{ 192, 192, 96 },
		ProbeGridPreset{ 256, 256, 128 },
		ProbeGridPreset{ 384, 384, 192 },
		ProbeGridPreset{ 512, 512, 256 },
	};

	uint ClampProbeGridQuality(uint a_quality)
	{
		return std::min<uint>(a_quality, static_cast<uint>(kProbeGridPresets.size() - 1));
	}

	uint ClampStableSliceCount(uint a_sliceCount, uint a_maxSlices)
	{
		return std::clamp(a_sliceCount, 1u, std::max(1u, a_maxSlices));
	}

	uint ClampUpdateInterval(uint a_interval)
	{
		return std::clamp(a_interval, 1u, 32u);
	}

	float ClampProbeFieldSize(float a_size)
	{
		// std::clamp passes NaN through unchanged; reject non-finite config values first.
		if (!std::isfinite(a_size))
			return Skylighting::Settings::kDefaultProbeFieldSize;

		return std::clamp(a_size, Skylighting::Settings::kMinProbeFieldSize, Skylighting::Settings::kMaxProbeFieldSize);
	}

	uint GetOcclusionUpdateInterval(const Skylighting::Settings& a_settings)
	{
		return a_settings.EnableReducedUpdateFrequency ? ClampUpdateInterval(a_settings.OcclusionUpdateInterval) : 1u;
	}

	// Probes integrate the occlusion map, so refreshing them faster than the map is wasted work.
	uint GetProbeUpdateInterval(const Skylighting::Settings& a_settings)
	{
		if (!a_settings.EnableReducedUpdateFrequency)
			return 1u;

		return std::max(ClampUpdateInterval(a_settings.ProbeUpdateInterval), ClampUpdateInterval(a_settings.OcclusionUpdateInterval));
	}

	bool UsesIncrementalProbeSlices(const Skylighting::Settings& a_settings, uint a_probeDepth)
	{
		return a_settings.EnableIncrementalProbeUpdates &&
		       ClampStableSliceCount(a_settings.StableSliceCount, a_probeDepth) < a_probeDepth;
	}

	uint GetOcclusionCornerBit(uint a_frameCount)
	{
		return 1u << (a_frameCount % kOcclusionCornerCount);
	}

	void ApplyOcclusionCornerFrustum(uint a_corner, RE::NiFrustum& a_frustum)
	{
		const float frustumSize = a_frustum.fTop;

		a_frustum.fBottom = (a_corner == 0 || a_corner == 1) ? -frustumSize : 0.0f;
		a_frustum.fLeft = (a_corner == 0 || a_corner == 2) ? -frustumSize : 0.0f;
		a_frustum.fRight = (a_corner == 1 || a_corner == 3) ? frustumSize : 0.0f;
		a_frustum.fTop = (a_corner == 2 || a_corner == 3) ? frustumSize : 0.0f;
	}

	bool ShouldRunPeriodicUpdate(uint& a_frameCounter, uint a_interval, bool a_forceRun)
	{
		const bool shouldRun = a_forceRun || ((a_frameCounter % a_interval) == 0);
		a_frameCounter++;
		return shouldRun;
	}

	void NormalizeSettings(Skylighting::Settings& a_settings)
	{
		a_settings.StableSliceCount = std::max(1u, a_settings.StableSliceCount);
		a_settings.OcclusionUpdateInterval = ClampUpdateInterval(a_settings.OcclusionUpdateInterval);
		a_settings.ProbeUpdateInterval = ClampUpdateInterval(a_settings.ProbeUpdateInterval);
		a_settings.ProbeGridQuality = ClampProbeGridQuality(a_settings.ProbeGridQuality);
		a_settings.ProbeFieldSize = ClampProbeFieldSize(a_settings.ProbeFieldSize);
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skylighting::Settings,
	MaxZenith,
	MinDiffuseVisibility,
	MinSpecularVisibility,
	ProbeFieldSize,
	ProbeGridQuality,
	EnableIncrementalProbeUpdates,
	StableSliceCount,
	EnableReducedUpdateFrequency,
	OcclusionUpdateInterval,
	ProbeUpdateInterval)

void Skylighting::LoadSettings(json& o_json)
{
	settings = o_json;
	NormalizeSettings(settings);

	const uint previousDims[3] = { probeArrayDims[0], probeArrayDims[1], probeArrayDims[2] };
	ApplyProbeGridQuality();
	// Runtime profile loads can change the grid; queue the recreation for the
	// render thread since this runs on the main thread mid-frame.
	if (globals::d3d::device && globals::game::renderer &&
		(previousDims[0] != probeArrayDims[0] || previousDims[1] != probeArrayDims[1] || previousDims[2] != probeArrayDims[2])) {
		queuedSetupResources = true;
		queuedResetSkylighting = true;
	}
}

void Skylighting::SaveSettings(json& o_json)
{
	NormalizeSettings(settings);
	o_json = settings;
}

void Skylighting::RestoreDefaultSettings()
{
	const uint previousProbeGridQuality = settings.ProbeGridQuality;
	settings = {};
	ApplyProbeGridQuality();
	if (previousProbeGridQuality != settings.ProbeGridQuality)
		queuedSetupResources = true;
	queuedResetSkylighting = true;
}

void Skylighting::ResetSkylighting()
{
	if (texAccumFramesArray && texAccumFramesArray->uav) {
		auto context = globals::d3d::context;
		UINT clr[1] = { 0 };
		context->ClearUnorderedAccessViewUint(texAccumFramesArray->uav.get(), clr);
	}
	ResetProbeUpdateWindow();
	forcedFullUpdateFrames = 1;
	forceProbeUpdateThisFrame = true;
	probeUpdateFrameCounter = 0;
	occlusionUpdateFrameCounter = 0;
	queuedResetSkylighting = false;
}

void Skylighting::ResetProbeUpdateWindow()
{
	probeUpdateSliceCursor = 0;
	probeUpdateCornerMask = 0;
}

void Skylighting::ApplyProbeGridQuality()
{
	settings.ProbeGridQuality = ClampProbeGridQuality(settings.ProbeGridQuality);
	const auto& preset = kProbeGridPresets[settings.ProbeGridQuality];
	probeArrayDims[0] = preset.Width;
	probeArrayDims[1] = preset.Height;
	probeArrayDims[2] = preset.Depth;
	settings.StableSliceCount = ClampStableSliceCount(settings.StableSliceCount, probeArrayDims[2]);
}

void Skylighting::DrawSettings()
{
	ImGui::Text("%s", T(TKEY("min_visibility_desc"), "Minimum visibility values. Diffuse darkens objects. Specular removes the sky from reflections."));
	ImGui::SliderFloat(T(TKEY("diffuse_min_visibility"), "Diffuse Min Visibility"), &settings.MinDiffuseVisibility, 0.01f, 1.f, "%.2f");
	ImGui::SliderFloat(T(TKEY("specular_min_visibility"), "Specular Min Visibility"), &settings.MinSpecularVisibility, 0.01f, 1.f, "%.2f");

	ImGui::Separator();

	if (ImGui::Button(T(TKEY("rebuild"), "Rebuild Skylighting")))
		ResetSkylighting();

	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("rebuild_tooltip"), "Changes below require rebuilding, a loading screen, or moving away from the current location to apply."));

	ImGui::Separator();
	ImGui::Text("%s", T(TKEY("performance_options"), "Performance options"));

	const char* probeGridLabels[] = {
		T(TKEY("grid_quality_performance"), "Performance (128 x 128 x 64)"),
		T(TKEY("grid_quality_balanced"), "Balanced (192 x 192 x 96)"),
		T(TKEY("grid_quality_quality"), "Quality (256 x 256 x 128)"),
		T(TKEY("grid_quality_ultra"), "Ultra Quality (384 x 384 x 192)"),
		T(TKEY("grid_quality_extreme"), "Extreme (512 x 512 x 256)"),
	};
	static_assert(std::size(probeGridLabels) == kProbeGridPresets.size());

	int probeGridQualityUI = static_cast<int>(ClampProbeGridQuality(settings.ProbeGridQuality));
	if (ImGui::Combo(T(TKEY("probe_grid_quality"), "Probe Grid Quality"), &probeGridQualityUI, probeGridLabels, static_cast<int>(std::size(probeGridLabels)))) {
		const uint nextQuality = ClampProbeGridQuality(static_cast<uint>(std::max(probeGridQualityUI, 0)));
		if (settings.ProbeGridQuality != nextQuality) {
			settings.ProbeGridQuality = nextQuality;
			ApplyProbeGridQuality();
			SetupResources();
			ResetSkylighting();
		}
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("probe_grid_quality_tooltip"), "Probe grid resolution. Higher tiers add detail and cost; Extreme uses roughly 576 MB of VRAM."));
	ImGui::Text(T(TKEY("active_probe_grid"), "Active Probe Grid: %u x %u x %u"), probeArrayDims[0], probeArrayDims[1], probeArrayDims[2]);

	ImGui::Checkbox(T(TKEY("reduced_update_frequency"), "Enable Reduced Update Frequency"), &settings.EnableReducedUpdateFrequency);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("reduced_update_frequency_tooltip"), "Reuses occlusion and probe data for several frames before refreshing. Faster, but reacts slower."));

	NormalizeSettings(settings);
	settings.StableSliceCount = ClampStableSliceCount(settings.StableSliceCount, probeArrayDims[2]);
	bool usesIncrementalProbeSlices = UsesIncrementalProbeSlices(settings, probeArrayDims[2]);

	ImGui::BeginDisabled(!settings.EnableReducedUpdateFrequency);
	{
		int occlusionIntervalUI = static_cast<int>(settings.OcclusionUpdateInterval);
		if (ImGui::SliderInt(T(TKEY("occlusion_update_interval"), "Occlusion Update Interval"), &occlusionIntervalUI, 1, 16))
			settings.OcclusionUpdateInterval = ClampUpdateInterval(static_cast<uint>(occlusionIntervalUI));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("occlusion_update_interval_tooltip"), "How often skylight shadowing refreshes. 1 = every frame."));

		ImGui::BeginDisabled(usesIncrementalProbeSlices);
		int probeIntervalUI = static_cast<int>(settings.ProbeUpdateInterval);
		if (ImGui::SliderInt(T(TKEY("probe_update_interval"), "Probe Update Interval"), &probeIntervalUI, 1, 16))
			settings.ProbeUpdateInterval = ClampUpdateInterval(static_cast<uint>(probeIntervalUI));
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", usesIncrementalProbeSlices ?
								  T(TKEY("probe_update_interval_tooltip_incremental"), "Follows the skylight shadow refresh automatically while incremental updates are on.") :
								  T(TKEY("probe_update_interval_tooltip"), "How often probe data refreshes. 1 = every frame."));
	}
	ImGui::EndDisabled();
	NormalizeSettings(settings);

	const bool previousIncrementalProbeUpdates = settings.EnableIncrementalProbeUpdates;
	if (ImGui::Checkbox(T(TKEY("incremental_probe_updates"), "Enable Incremental Probe Updates"), &settings.EnableIncrementalProbeUpdates) &&
		previousIncrementalProbeUpdates != settings.EnableIncrementalProbeUpdates) {
		ResetProbeUpdateWindow();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("incremental_probe_updates_tooltip"), "Spreads probe updates over multiple frames to smooth spikes."));

	ImGui::BeginDisabled(!settings.EnableIncrementalProbeUpdates);
	{
		int stableSliceCountUI = static_cast<int>(settings.StableSliceCount);
		if (ImGui::SliderInt(T(TKEY("stable_slice_count"), "Stable Slice Count"), &stableSliceCountUI, 1, static_cast<int>(probeArrayDims[2]))) {
			const uint nextStableSliceCount = ClampStableSliceCount(static_cast<uint>(stableSliceCountUI), probeArrayDims[2]);
			if (settings.StableSliceCount != nextStableSliceCount) {
				settings.StableSliceCount = nextStableSliceCount;
				ResetProbeUpdateWindow();
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("stable_slice_count_tooltip"), "Lower = smoother performance but slower to settle. Higher = reacts faster with more cost."));
	}
	ImGui::EndDisabled();

	usesIncrementalProbeSlices = UsesIncrementalProbeSlices(settings, probeArrayDims[2]);
	const uint stableSliceBatches = (probeArrayDims[2] + settings.StableSliceCount - 1) / settings.StableSliceCount;
	const uint stableRefreshFrames = usesIncrementalProbeSlices ?
	                                     stableSliceBatches * kOcclusionCornerCount * GetOcclusionUpdateInterval(settings) :
	                                     GetProbeUpdateInterval(settings);
	ImGui::Text(T(TKEY("stable_refresh_estimate"), "Stable probe field full refresh: ~%u frame(s)"), stableRefreshFrames);

	float probeFieldSizeUI = ClampProbeFieldSize(settings.ProbeFieldSize);
	if (ImGui::SliderFloat(T(TKEY("probe_field_size"), "Skylighting Distance"), &probeFieldSizeUI, Skylighting::Settings::kMinProbeFieldSize, Skylighting::Settings::kMaxProbeFieldSize, "%.0f units", ImGuiSliderFlags_AlwaysClamp)) {
		settings.ProbeFieldSize = ClampProbeFieldSize(probeFieldSizeUI);
		ResetSkylighting();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("probe_field_size_tooltip"),
							  "How far skylighting reaches around you (about half this distance in each\n"
							  "direction). Higher values reach farther but lose detail; raise Probe Grid\n"
							  "Quality to compensate."));
	}

	ImGui::Separator();

	ImGui::SliderAngle(T(TKEY("max_zenith"), "Max Zenith Angle"), &settings.MaxZenith, 0, 90);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("max_zenith_tooltip"), "Smaller angles creates more focused top-down shadow."));
}

void Skylighting::SetupResources()
{
	ApplyProbeGridQuality();

	// Re-entrant: probe grid preset changes recreate the arrays at new dimensions.
	delete texOcclusion;
	texOcclusion = nullptr;
	delete texProbeArray;
	texProbeArray = nullptr;
	delete texAccumFramesArray;
	texAccumFramesArray = nullptr;
	comparisonSampler = nullptr;

	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(&texDesc);
		precipitationOcclusion.depthSRV->GetDesc(&srvDesc);
		precipitationOcclusion.views[0]->GetDesc(&dsvDesc);

		texOcclusion = new Texture2D(texDesc, "Skylighting::Occlusion");
		texOcclusion->CreateSRV(srvDesc);
		texOcclusion->CreateDSV(dsvDesc);
	}

	{
		D3D11_TEXTURE3D_DESC texDesc{
			.Width = probeArrayDims[0],
			.Height = probeArrayDims[1],
			.Depth = probeArrayDims[2],
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

		texProbeArray = new Texture3D(texDesc, "Skylighting::ProbeArray");
		texProbeArray->CreateSRV(srvDesc);
		texProbeArray->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UINT;

		texAccumFramesArray = new Texture3D(texDesc, "Skylighting::AccumFramesArray");
		texAccumFramesArray->CreateSRV(srvDesc);
		texAccumFramesArray->CreateUAV(uavDesc);
	}

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

void Skylighting::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&probeUpdateCompute
	};

	for (auto shader : shaderPtrs)
		shader = nullptr;

	CompileComputeShaders();
}

void Skylighting::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &probeUpdateCompute, "UpdateProbesCS.hlsl", {} },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\Skylighting") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}
}

Skylighting::SkylightingCB Skylighting::GetCommonBufferData(bool a_inWorld)
{
	if (!a_inWorld)
		return Skylighting::SkylightingCB{};

	if (globals::state->isMapMenuOpen)
		return Skylighting::SkylightingCB{};

	auto eyePosNI = Util::GetEyePosition(0);
	auto eyePos = float3{ eyePosNI.x, eyePosNI.y, eyePosNI.z };
	const float probeFieldSize = ClampProbeFieldSize(settings.ProbeFieldSize);

	float3 cellSize = {
		probeFieldSize / probeArrayDims[0],
		probeFieldSize / probeArrayDims[1],
		probeFieldSize * .5f / probeArrayDims[2]
	};
	auto cellID = eyePos / cellSize;
	cellID = { round(cellID.x), round(cellID.y), round(cellID.z) };
	auto cellOrigin = cellID * cellSize;
	float3 cellIDDiff = prevCellID - cellID;
	prevCellID = cellID;
	DirectX::XMINT3 cellIDDiffI = { static_cast<int>(cellIDDiff.x), static_cast<int>(cellIDDiff.y), static_cast<int>(cellIDDiff.z) };

	bool shouldForceFullUpdate =
		cellIDDiffI.x != 0 ||
		cellIDDiffI.y != 0 ||
		cellIDDiffI.z != 0 ||
		forcedFullUpdateFrames > 0;
	forceProbeUpdateThisFrame = shouldForceFullUpdate;

	probeUpdateSliceStart = 0;
	probeUpdateSliceCount = probeArrayDims[2];

	if (UsesIncrementalProbeSlices(settings, probeArrayDims[2]) && !shouldForceFullUpdate) {
		uint stableSliceCount = ClampStableSliceCount(settings.StableSliceCount, probeArrayDims[2]);

		probeUpdateSliceStart = probeUpdateSliceCursor;
		probeUpdateSliceCount = std::min(stableSliceCount, probeArrayDims[2] - probeUpdateSliceStart);
	} else {
		ResetProbeUpdateWindow();
	}

	if (forcedFullUpdateFrames > 0)
		forcedFullUpdateFrames--;

	return {
		.OcclusionViewProj = OcclusionTransform,
		.OcclusionDir = OcclusionDir,
		.PosOffset = cellOrigin - eyePos,
		.ArrayOrigin = {
			(static_cast<int>(cellID.x) - probeArrayDims[0] / 2) % probeArrayDims[0],
			(static_cast<int>(cellID.y) - probeArrayDims[1] / 2) % probeArrayDims[1],
			(static_cast<int>(cellID.z) - probeArrayDims[2] / 2) % probeArrayDims[2] },
		.ValidMargin = { cellIDDiffI.x, cellIDDiffI.y, cellIDDiffI.z },
		.ArrayDims = { probeArrayDims[0], probeArrayDims[1], probeArrayDims[2] },
		.ProbeFieldSize = probeFieldSize,
		.MinDiffuseVisibility = settings.MinDiffuseVisibility,
		.MinSpecularVisibility = settings.MinSpecularVisibility,
		.ProbeUpdateSliceStart = probeUpdateSliceStart,
		.ProbeUpdateSliceCount = probeUpdateSliceCount
	};
}

void Skylighting::Prepass()
{
	// Render thread: safe point to recreate resources queued by settings loads.
	if (queuedSetupResources && globals::d3d::device && globals::game::renderer) {
		queuedSetupResources = false;
		SetupResources();
	}

	if (globals::state->isMapMenuOpen)
		return;

	bool interior = true;

	if (auto sky = globals::game::sky)
		interior = sky->mode.get() != RE::Sky::Mode::kFull;

	if (interior)
		return;

	CS_GPU_PASS("Skylighting::ProbeUpdate");

	auto context = globals::d3d::context;

	{
		std::array<ID3D11ShaderResourceView*, 1> srvs = { texOcclusion->srv.get() };
		std::array<ID3D11UnorderedAccessView*, 2> uavs = { texProbeArray->uav.get(), texAccumFramesArray->uav.get() };
		std::array<ID3D11SamplerState*, 1> samplers = { comparisonSampler.get() };

		// Update probe array
		{
			const bool updatingIncrementalSlices = UsesIncrementalProbeSlices(settings, probeArrayDims[2]) && probeUpdateSliceCount < probeArrayDims[2];
			const uint occlusionCornerBit = GetOcclusionCornerBit(frameCount);

			bool shouldUpdateProbes = false;
			if (updatingIncrementalSlices) {
				shouldUpdateProbes = forceProbeUpdateThisFrame || ((probeUpdateCornerMask & occlusionCornerBit) == 0);
			} else {
				const uint probeUpdateInterval = GetProbeUpdateInterval(settings);
				shouldUpdateProbes = ShouldRunPeriodicUpdate(probeUpdateFrameCounter, probeUpdateInterval, forceProbeUpdateThisFrame);
			}

			uint dispatchSliceCount = std::clamp(probeUpdateSliceCount, 1u, probeArrayDims[2]);

			if (shouldUpdateProbes) {
				context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
				context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
				context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
				context->CSSetShader(probeUpdateCompute.get(), nullptr, 0);
				context->Dispatch((probeArrayDims[0] + 7u) >> 3, (probeArrayDims[1] + 7u) >> 3, dispatchSliceCount);

				if (updatingIncrementalSlices) {
					probeUpdateCornerMask |= occlusionCornerBit;

					// The occlusion map covers one XY quadrant per render; advance the slice
					// window only after all quadrants refreshed, else batches leave holes.
					if ((probeUpdateCornerMask & kAllOcclusionCornersMask) == kAllOcclusionCornersMask) {
						probeUpdateSliceCursor += probeUpdateSliceCount;
						if (probeUpdateSliceCursor >= probeArrayDims[2])
							probeUpdateSliceCursor = 0;
						probeUpdateCornerMask = 0;
					}
				} else {
					ResetProbeUpdateWindow();
				}
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
		ID3D11ShaderResourceView* srv = texProbeArray->srv.get();
		context->PSSetShaderResources(50, 1, &srv);
	}
}

void Skylighting::PostPostLoad()
{
	logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
	stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
	stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));

	if (globals::game::isVR)
		stl::write_thunk_call<SetViewFrustumVR>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));
	else
		stl::write_thunk_call<SetViewFrustum>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));

	MenuOpenCloseEventHandler::Register();
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

	if (skylighting.inOcclusion) {
		if (auto userData = geometry->GetUserData()) {
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

	bool valid = false;

	if (skylighting.inOcclusion) {
		valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
	} else {
		valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
	}

	if (valid) {
		if (geometry->worldBound.radius > 32) {
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
		}
	}
	return precipitationOcclusionMapRenderPassList;
}

void Skylighting::SetViewFrustum::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		ApplyOcclusionCornerFrustum(skylighting.frameCount % kOcclusionCornerCount, *a_frustum);
	}

	func(a_camera, a_frustum);
}

void Skylighting::SetViewFrustumVR::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum, uint a_eyeIndex)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		ApplyOcclusionCornerFrustum(skylighting.frameCount % kOcclusionCornerCount, *a_frustum);
	}

	func(a_camera, a_frustum, a_eyeIndex);
}

void Skylighting::RenderOcclusion()
{
	ZoneScopedS(8);
	auto shaderCache = globals::shaderCache;
	auto renderer = globals::game::renderer;
	auto sky = globals::game::sky;

	if (!shaderCache->IsEnabled()) {
		{
			CS_GPU_PASS("Skylighting::PrecipitationMask");
			Main_Precipitation_RenderOcclusion::func();
		}
		return;
	}

	if (sky) {
		if (!Util::IsInterior()) {
			static bool doPrecip = false;

			auto precip = sky->precip;
			if (!precip)
				return;

			{
				CS_GPU_PASS("Skylighting::PrecipitationMask");

				doPrecip = false;

				auto precipObject = precip->currentPrecip;
				if (!precipObject) {
					precipObject = precip->lastPrecip;
				}
				if (precipObject) {
					precip->SetupMask();
					auto& effect = precipObject->GetGeometryRuntimeData().shaderProperty;
					auto shaderProp = effect.get();
					auto particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(shaderProp);
					if (particleShaderProperty) {
						auto rain = (RE::BSParticleShaderRainEmitter*)(particleShaderProperty->particleEmitter);
						precip->RenderMask(rain);
					}
				}
			}

			// The engine can drop the occlusion camera during weather teardown; skip the
			// custom projection pass rather than dereferencing a null camera.
			auto occlusionCamera = precip->occlusionData.camera;
			if (!occlusionCamera)
				return;

			{
				CS_GPU_PASS("Skylighting::SkylightingMask");

				const bool forceOcclusionRefresh = queuedResetSkylighting;
				if (queuedResetSkylighting)
					ResetSkylighting();

				const bool shouldUpdateOcclusion = ShouldRunPeriodicUpdate(occlusionUpdateFrameCounter, GetOcclusionUpdateInterval(settings), forceOcclusionRefresh);
				if (!shouldUpdateOcclusion)
					return;

				frameCount++;

				auto& precipitation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
				RE::BSGraphics::DepthStencilData precipitationCopy = precipitation;

				precipitation.depthSRV = texOcclusion->srv.get();
				precipitation.texture = texOcclusion->resource.get();
				precipitation.views[0] = texOcclusion->dsv.get();

				static float& PrecipitationShaderCubeSize = (*(float*)REL::RelocationID(515451, 401590).address());
				float originalPrecipitationShaderCubeSize = PrecipitationShaderCubeSize;

				static RE::NiPoint3& PrecipitationShaderDirection = (*(RE::NiPoint3*)REL::RelocationID(515509, 401648).address());
				RE::NiPoint3 originalParticleShaderDirection = PrecipitationShaderDirection;

				inOcclusion = true;
				PrecipitationShaderCubeSize = ClampProbeFieldSize(settings.ProbeFieldSize);

				float originaLastCubeSize = precip->lastCubeSize;
				precip->lastCubeSize = PrecipitationShaderCubeSize;

				float2 vPoint;
				{
					constexpr float rcpRandMax = 1.f / RAND_MAX;
					static int randSeed = std::rand();
					static uint randFrameCount = 0;

					// r2 sequence
					vPoint = float2(randSeed * rcpRandMax) + (float)randFrameCount * float2(0.245122333753f, 0.430159709002f);
					vPoint.x -= static_cast<unsigned long long>(vPoint.x);
					vPoint.y -= static_cast<unsigned long long>(vPoint.y);

					randFrameCount++;
					if (randFrameCount == 1000) {
						randFrameCount = 0;
						randSeed = std::rand();
					}

					// disc transformation
					vPoint.x = sqrt(vPoint.x * sin(settings.MaxZenith));
					vPoint.y *= 6.28318530718f;

					vPoint = { vPoint.x * cos(vPoint.y), vPoint.x * sin(vPoint.y) };
				}

				float3 PrecipitationShaderDirectionF = -float3{ vPoint.x, vPoint.y, sqrt(1 - vPoint.LengthSquared()) };
				PrecipitationShaderDirectionF.Normalize();

				PrecipitationShaderDirection = { PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z };

				static REL::Relocation<void(RE::Precipitation*, RE::NiPointer<RE::NiCamera>)> _computeProjection{ REL::RelocationID(25643, 26185) };
				{
					ZoneScopedN("Skylighting - Setup Projection");
					_computeProjection(precip, occlusionCamera);
					precip->SetupMask();
				}

				BSParticleShaderRainEmitter* rain = new BSParticleShaderRainEmitter;
				{
					CS_GPU_PASS("Skylighting::OcclusionMask");
					precip->RenderMask((RE::BSParticleShaderRainEmitter*)rain);
				}
				inOcclusion = false;

				OcclusionDir = -float4{ PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z, 0 };
				OcclusionTransform = ((RE::BSParticleShaderRainEmitter*)rain)->occlusionProjection;

				delete rain;

				PrecipitationShaderCubeSize = originalPrecipitationShaderCubeSize;
				precip->lastCubeSize = originaLastCubeSize;

				PrecipitationShaderDirection = originalParticleShaderDirection;

				precipitation = precipitationCopy;

				{
					ZoneScopedN("Skylighting - Restore Projection");
					_computeProjection(precip, occlusionCamera);
				}
			}
		}
	}
}

void Skylighting::Main_Precipitation_RenderOcclusion::thunk()
{
	globals::features::skylighting.RenderOcclusion();
}

RE::BSEventNotifyControl Skylighting::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// When entering a new cell through a loadscreen, update every frame until completion
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		if (!a_event->opening)
			globals::features::skylighting.queuedResetSkylighting = true;
	}

	return RE::BSEventNotifyControl::kContinue;
}
#undef I18N_KEY_PREFIX
