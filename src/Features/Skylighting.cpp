#include "Skylighting.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <array>

#include "ShaderCache.h"
#include "State.h"

namespace
{
	struct ProbeGridPreset
	{
		uint Width;
		uint Height;
		uint Depth;
		const char* Label;
	};

	constexpr std::array<ProbeGridPreset, 3> kProbeGridPresets = {
		ProbeGridPreset{ 128, 128, 64, "Performance (128 x 128 x 64)" },
		ProbeGridPreset{ 192, 192, 96, "Balanced (192 x 192 x 96)" },
		ProbeGridPreset{ 256, 256, 128, "Quality (256 x 256 x 128)" },
	};

	uint ClampProbeGridQuality(uint a_quality)
	{
		return std::min<uint>(a_quality, static_cast<uint>(kProbeGridPresets.size() - 1));
	}

	const ProbeGridPreset& GetProbeGridPreset(uint a_quality)
	{
		return kProbeGridPresets[ClampProbeGridQuality(a_quality)];
	}

	float4 EvaluateDirectionalSHBasis4Pi(const float3& a_direction)
	{
		// Keep in sync with SphericalHarmonics::Evaluate in package/Shaders/Common/Spherical Harmonics/SphericalHarmonics.hlsli.
		constexpr float shL0 = 0.28209479177387814347f;
		constexpr float shL1 = 0.48860251190291992159f;
		constexpr float fourPi = 12.56637061435917295385f;
		return {
			shL0 * fourPi,
			-shL1 * a_direction.y * fourPi,
			shL1 * a_direction.z * fourPi,
			-shL1 * a_direction.x * fourPi
		};
	}

	uint ClampStableSliceCount(uint a_sliceCount, uint a_maxSlices)
	{
		const uint maxSlices = std::max(1u, a_maxSlices);
		return std::clamp(a_sliceCount, 1u, maxSlices);
	}

	uint ClampUpdateInterval(uint a_interval)
	{
		return std::clamp(a_interval, 1u, 32u);
	}

	uint GetOcclusionUpdateInterval(const Skylighting::Settings& a_settings)
	{
		return a_settings.EnableReducedUpdateFrequency ? ClampUpdateInterval(a_settings.OcclusionUpdateInterval) : 1u;
	}

	uint GetProbeUpdateInterval(const Skylighting::Settings& a_settings)
	{
		if (!a_settings.EnableReducedUpdateFrequency)
			return 1u;

		return std::max(ClampUpdateInterval(a_settings.ProbeUpdateInterval), ClampUpdateInterval(a_settings.OcclusionUpdateInterval));
	}

	uint WrapIndex(int a_value, uint a_modulus)
	{
		const int modulus = std::max(1, static_cast<int>(a_modulus));
		int wrapped = a_value % modulus;
		if (wrapped < 0)
			wrapped += modulus;
		return static_cast<uint>(wrapped);
	}

	bool ShouldRunPeriodicUpdate(uint& a_frameCounter, uint a_interval, bool a_forceRun)
	{
		const bool shouldRun = a_forceRun || ((a_frameCounter % a_interval) == 0);
		a_frameCounter++;
		return shouldRun;
	}

	void ApplyPlatformDefaults(Skylighting::Settings& a_settings)
	{
		a_settings = {};
		if (REL::Module::IsVR()) {
			// VR is significantly more sensitive to both probe-update and occlusion cadence.
			a_settings.ProbeGridQuality = 1;
			a_settings.EnableIncrementalProbeUpdates = true;
			a_settings.EnableReducedUpdateFrequency = true;
			a_settings.EnableFastProbeSampling = true;
		}
	}

	void NormalizeSettingsForRuntime(Skylighting::Settings& a_settings)
	{
		a_settings.ProbeGridQuality = ClampProbeGridQuality(a_settings.ProbeGridQuality);
		a_settings.OcclusionUpdateInterval = ClampUpdateInterval(a_settings.OcclusionUpdateInterval);
		a_settings.ProbeUpdateInterval = ClampUpdateInterval(a_settings.ProbeUpdateInterval);
		if (a_settings.ProbeUpdateInterval < a_settings.OcclusionUpdateInterval)
			a_settings.ProbeUpdateInterval = a_settings.OcclusionUpdateInterval;
	}

	template <class T>
	void LoadIfPresent(const json& a_json, const char* a_key, T& a_value)
	{
		if (auto it = a_json.find(a_key); it != a_json.end() && !it->is_null())
			a_value = it->get<T>();
	}

	RE::NiPointer<RE::BSGeometry> GetActivePrecipitationObject(RE::Precipitation* a_precip)
	{
		if (!a_precip)
			return nullptr;
		if (a_precip->currentPrecip)
			return a_precip->currentPrecip;
		return a_precip->lastPrecip;
	}

	RE::BSParticleShaderRainEmitter* GetRainEmitter(const RE::NiPointer<RE::BSGeometry>& a_precipObject)
	{
		if (!a_precipObject)
			return nullptr;

		auto* shaderProp = a_precipObject->GetGeometryRuntimeData().shaderProperty.get();
		auto* particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(shaderProp);
		if (!particleShaderProperty || !particleShaderProperty->particleEmitter)
			return nullptr;

		return static_cast<RE::BSParticleShaderRainEmitter*>(particleShaderProperty->particleEmitter);
	}
}

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
	EnableFastProbeSampling)

void Skylighting::LoadSettings(json& o_json)
{
	ApplyPlatformDefaults(settings);

	LoadIfPresent(o_json, "MaxZenith", settings.MaxZenith);
	LoadIfPresent(o_json, "MinDiffuseVisibility", settings.MinDiffuseVisibility);
	LoadIfPresent(o_json, "MinSpecularVisibility", settings.MinSpecularVisibility);
	LoadIfPresent(o_json, "ProbeGridQuality", settings.ProbeGridQuality);
	LoadIfPresent(o_json, "EnableIncrementalProbeUpdates", settings.EnableIncrementalProbeUpdates);
	LoadIfPresent(o_json, "StableSliceCount", settings.StableSliceCount);
	LoadIfPresent(o_json, "EnableReducedUpdateFrequency", settings.EnableReducedUpdateFrequency);
	LoadIfPresent(o_json, "OcclusionUpdateInterval", settings.OcclusionUpdateInterval);
	LoadIfPresent(o_json, "ProbeUpdateInterval", settings.ProbeUpdateInterval);
	LoadIfPresent(o_json, "EnableFastProbeSampling", settings.EnableFastProbeSampling);

	NormalizeSettingsForRuntime(settings);
	ApplyProbeGridQuality();
}

void Skylighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skylighting::RestoreDefaultSettings()
{
	ApplyPlatformDefaults(settings);
	NormalizeSettingsForRuntime(settings);
	ApplyProbeGridQuality();
}

void Skylighting::ApplyProbeGridQuality()
{
	settings.ProbeGridQuality = ClampProbeGridQuality(settings.ProbeGridQuality);
	const auto& preset = GetProbeGridPreset(settings.ProbeGridQuality);
	probeArrayDims[0] = preset.Width;
	probeArrayDims[1] = preset.Height;
	probeArrayDims[2] = preset.Depth;
	settings.StableSliceCount = ClampStableSliceCount(settings.StableSliceCount, probeArrayDims[2]);
}

void Skylighting::ResetSkylighting()
{
	if (texAccumFramesArray && texAccumFramesArray->uav) {
		auto context = globals::d3d::context;
		UINT clr[1] = { 0 };
		context->ClearUnorderedAccessViewUint(texAccumFramesArray->uav.get(), clr);
	}
	probeUpdateSliceCursor = 0;
	forcedFullUpdateFrames = 1;
	forceProbeUpdateThisFrame = true;
	probeUpdateFrameCounter = 0;
	occlusionUpdateFrameCounter = 0;
	queuedResetSkylighting = false;
}

void Skylighting::DrawSettings()
{
	ImGui::Text("Minimum visibility values. Diffuse darkens objects. Specular removes the sky from reflections.");
	ImGui::SliderFloat("Diffuse Min Visibility", &settings.MinDiffuseVisibility, 0.01f, 1.f, "%.2f");
	ImGui::SliderFloat("Specular Min Visibility", &settings.MinSpecularVisibility, 0.01f, 1.f, "%.2f");

	ImGui::Separator();

	if (ImGui::Button("Rebuild Skylighting"))
		ResetSkylighting();

	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Changes below require rebuilding, a loading screen, or moving away from the current location to apply.");

	ImGui::Separator();
	ImGui::Text("Performance options (highest impact first)");
	settings.ProbeGridQuality = ClampProbeGridQuality(settings.ProbeGridQuality);

	int probeGridQualityUI = static_cast<int>(settings.ProbeGridQuality);
	if (ImGui::BeginCombo("Probe Grid Quality", GetProbeGridPreset(settings.ProbeGridQuality).Label)) {
		for (uint quality = 0; quality < kProbeGridPresets.size(); quality++) {
			const bool isSelected = (probeGridQualityUI == static_cast<int>(quality));
			if (ImGui::Selectable(kProbeGridPresets[quality].Label, isSelected))
				probeGridQualityUI = static_cast<int>(quality);
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Main quality/performance switch. Performance is fastest, Quality is most detailed.");

	probeGridQualityUI = std::max(0, std::min(probeGridQualityUI, static_cast<int>(kProbeGridPresets.size() - 1)));
	if (settings.ProbeGridQuality != static_cast<uint>(probeGridQualityUI)) {
		settings.ProbeGridQuality = static_cast<uint>(probeGridQualityUI);
		ApplyProbeGridQuality();
		SetupResources();
		ResetSkylighting();
	}
	ImGui::Text("Active Probe Grid: %u x %u x %u", probeArrayDims[0], probeArrayDims[1], probeArrayDims[2]);

	ImGui::Separator();
	ImGui::Checkbox("Enable Reduced Update Frequency", &settings.EnableReducedUpdateFrequency);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Updates skylighting less often for a bigger FPS gain. Higher values can react a bit slower.");

	NormalizeSettingsForRuntime(settings);

	ImGui::BeginDisabled(!settings.EnableReducedUpdateFrequency);
	{
		int occlusionIntervalUI = static_cast<int>(settings.OcclusionUpdateInterval);
		if (ImGui::SliderInt("Occlusion Update Interval", &occlusionIntervalUI, 1, 16))
			settings.OcclusionUpdateInterval = ClampUpdateInterval(static_cast<uint>(occlusionIntervalUI));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How often skylight shadowing refreshes. 1 = every frame. Higher = faster, but slower reaction.");

		int probeIntervalUI = static_cast<int>(settings.ProbeUpdateInterval);
		if (ImGui::SliderInt("Probe Update Interval", &probeIntervalUI, 1, 16))
			settings.ProbeUpdateInterval = ClampUpdateInterval(static_cast<uint>(probeIntervalUI));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How often skylight data refreshes. 1 = every frame. Higher = faster, but slower reaction.");
	}
	ImGui::EndDisabled();
	NormalizeSettingsForRuntime(settings);

	if (settings.EnableReducedUpdateFrequency) {
		ImGui::Text("Occlusion refresh cadence: every %u frame(s)", settings.OcclusionUpdateInterval);
		ImGui::Text("Probe refresh cadence: every %u frame(s)", settings.ProbeUpdateInterval);
	}

	ImGui::Separator();
	ImGui::Checkbox("Enable Incremental Probe Updates", &settings.EnableIncrementalProbeUpdates);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Spreads skylighting work over multiple frames to smooth spikes.");

	uint stableSliceCount = ClampStableSliceCount(settings.StableSliceCount, probeArrayDims[2]);
	settings.StableSliceCount = stableSliceCount;

	ImGui::BeginDisabled(!settings.EnableIncrementalProbeUpdates);
	{
		int stableSliceCountUI = static_cast<int>(stableSliceCount);
		if (ImGui::SliderInt("Stable Slice Count", &stableSliceCountUI, 1, static_cast<int>(probeArrayDims[2])))
			settings.StableSliceCount = ClampStableSliceCount(static_cast<uint>(stableSliceCountUI), probeArrayDims[2]);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Lower = smoother performance but takes longer to settle. Higher = reacts faster with more cost.");
	}
	ImGui::EndDisabled();
	const uint stableRefreshFrames = (probeArrayDims[2] + settings.StableSliceCount - 1) / settings.StableSliceCount;
	ImGui::Text("Stable probe field full refresh: ~%u frame(s)", stableRefreshFrames);

	ImGui::Separator();
	ImGui::Checkbox("Enable Fast Probe Sampling", &settings.EnableFastProbeSampling);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Uses a lighter sampling mode. Usually faster, with slightly softer lighting detail.");

	ImGui::Separator();
	ImGui::SliderAngle("Max Zenith Angle", &settings.MaxZenith, 0, 90);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Smaller angles create a more focused top-down shadow.");
}

void Skylighting::SetupResources()
{
	ApplyProbeGridQuality();

	delete texOcclusion;
	texOcclusion = nullptr;
	delete texProbeArray;
	texProbeArray = nullptr;
	delete texAccumFramesArray;
	texAccumFramesArray = nullptr;

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

		texOcclusion = new Texture2D(texDesc);
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

		texProbeArray = new Texture3D(texDesc);
		texProbeArray->CreateSRV(srvDesc);
		texProbeArray->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UINT;

		texAccumFramesArray = new Texture3D(texDesc);
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
	}

	{
		DirectX::CreateDDSTextureFromFile(device, globals::d3d::context, L"Data\\Shaders\\Skylighting\\SpatiotemporalBlueNoise\\stbn_vec3_2Dx1D_128x128x64.dds", nullptr, stbn_vec3_2Dx1D_128x128x64.put());
	}

	CompileComputeShaders();
}

void Skylighting::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&probeUpdateCompute
	};

	for (auto shader : shaderPtrs)
		*shader = nullptr;

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

	if (auto ui = globals::game::ui)
		if (ui->IsMenuOpen(RE::MapMenu::MENU_NAME))
			return Skylighting::SkylightingCB{};

	auto eyePosNI = Util::GetEyePosition(0);
	auto eyePos = float3{ eyePosNI.x, eyePosNI.y, eyePosNI.z };

	float3 cellSize = {
		occlusionDistance / probeArrayDims[0],
		occlusionDistance / probeArrayDims[1],
		occlusionDistance * .5f / probeArrayDims[2]
	};
	auto cellID = eyePos / cellSize;
	cellID = { round(cellID.x), round(cellID.y), round(cellID.z) };
	auto cellOrigin = cellID * cellSize;
	float3 cellIDDiff = prevCellID - cellID;
	prevCellID = cellID;
	DirectX::XMINT3 cellIDDiffI = { (int)cellIDDiff.x, (int)cellIDDiff.y, (int)cellIDDiff.z };

	bool shouldForceFullUpdate =
		cellIDDiffI.x != 0 ||
		cellIDDiffI.y != 0 ||
		cellIDDiffI.z != 0 ||
		forcedFullUpdateFrames > 0;
	forceProbeUpdateThisFrame = shouldForceFullUpdate;

	probeUpdateSliceStart = 0;
	probeUpdateSliceCount = probeArrayDims[2];

	if (settings.EnableIncrementalProbeUpdates && !shouldForceFullUpdate) {
		uint stableSliceCount = ClampStableSliceCount(settings.StableSliceCount, probeArrayDims[2]);

		probeUpdateSliceStart = probeUpdateSliceCursor;
		probeUpdateSliceCount = std::min(stableSliceCount, probeArrayDims[2] - probeUpdateSliceStart);
	} else {
		probeUpdateSliceCursor = 0;
	}

	if (forcedFullUpdateFrames > 0)
		forcedFullUpdateFrames--;

	return {
		.OcclusionViewProj = OcclusionTransform,
		.OcclusionSHBasis4Pi = occlusionSHBasis4Pi,
		.PosOffset = cellOrigin - eyePos,
		.FastSamplingMode = settings.EnableFastProbeSampling ? 1u : 0u,
		.ArrayOrigin = {
			WrapIndex(static_cast<int>(cellID.x) - static_cast<int>(probeArrayDims[0] / 2), probeArrayDims[0]),
			WrapIndex(static_cast<int>(cellID.y) - static_cast<int>(probeArrayDims[1] / 2), probeArrayDims[1]),
			WrapIndex(static_cast<int>(cellID.z) - static_cast<int>(probeArrayDims[2] / 2), probeArrayDims[2]) },
		.ValidMargin = { cellIDDiffI.x, cellIDDiffI.y, cellIDDiffI.z },
		.ArrayDims = { probeArrayDims[0], probeArrayDims[1], probeArrayDims[2] },
		.MinDiffuseVisibility = settings.MinDiffuseVisibility,
		.MinSpecularVisibility = settings.MinSpecularVisibility,
		.ProbeUpdateSliceStart = probeUpdateSliceStart,
		.ProbeUpdateSliceCount = probeUpdateSliceCount
	};
}

void Skylighting::Prepass()
{
	if (auto ui = globals::game::ui)
		if (ui->IsMenuOpen(RE::MapMenu::MENU_NAME))
			return;

	bool interior = true;

	if (auto sky = globals::game::sky)
		interior = sky->mode.get() != RE::Sky::Mode::kFull;

	if (interior)
		return;

	TracyD3D11Zone(globals::state->tracyCtx, "Skylighting - Update Probes");

	auto context = globals::d3d::context;

	{
		std::array<ID3D11ShaderResourceView*, 1> srvs = { texOcclusion->srv.get() };
		std::array<ID3D11UnorderedAccessView*, 2> uavs = { texProbeArray->uav.get(), texAccumFramesArray->uav.get() };
		std::array<ID3D11SamplerState*, 1> samplers = { comparisonSampler.get() };

		// Update probe array
		{
			const uint probeUpdateInterval = GetProbeUpdateInterval(settings);
			const bool shouldUpdateProbes = ShouldRunPeriodicUpdate(probeUpdateFrameCounter, probeUpdateInterval, forceProbeUpdateThisFrame);

			uint dispatchSliceCount = probeUpdateSliceCount == 0 ? 1 : probeUpdateSliceCount;
			if (dispatchSliceCount > probeArrayDims[2])
				dispatchSliceCount = probeArrayDims[2];

			if (shouldUpdateProbes) {
				context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
				context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
				context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
				context->CSSetShader(probeUpdateCompute.get(), nullptr, 0);
				context->Dispatch((probeArrayDims[0] + 7u) >> 3, (probeArrayDims[1] + 7u) >> 3, dispatchSliceCount);

				// Advance the rotating incremental window only when work actually executes.
				if (probeUpdateSliceCount < probeArrayDims[2]) {
					probeUpdateSliceCursor += probeUpdateSliceCount;
					if (probeUpdateSliceCursor >= probeArrayDims[2])
						probeUpdateSliceCursor = 0;
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
		ID3D11ShaderResourceView* srvs[2] = { texProbeArray->srv.get(), stbn_vec3_2Dx1D_128x128x64.get() };
		context->PSSetShaderResources(50, 2, srvs);
	}
}

void Skylighting::PostPostLoad()
{
	logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
	stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
	stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));

	if (REL::Module::IsVR())
		stl::write_thunk_call<SetViewFrustumVR>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));
	else
		stl::write_thunk_call<SetViewFrustum>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));

	MenuOpenCloseEventHandler::Register();
}

//////////////////////////////////////////////////////////////

struct RainEmitterProjectionCapture
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

	if (geometry->worldBound.radius <= 32)
		return precipitationOcclusionMapRenderPassList;

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
		stl::enumeration<RE::BSUtilityShader::Flags> technique;
		technique.set(RenderDepth);

		if (property->flags.any(kVertexColors)) {
			technique.set(Vc);
		}

		const auto alphaProperty = geometry->GetGeometryRuntimeData().alphaProperty.get();
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
	return precipitationOcclusionMapRenderPassList;
}

void Skylighting::SetViewFrustum::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		uint corner = skylighting.frameCount % 4;

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
		uint corner = skylighting.frameCount % 4;

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
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto sky = globals::game::sky;

	if (!shaderCache->IsEnabled()) {
		state->BeginPerfEvent("Precipitation Mask");
		Main_Precipitation_RenderOcclusion::func();
		state->EndPerfEvent();
		return;
	}

	if (sky) {
		if (!Util::IsInterior()) {
			auto precip = sky->precip;

			{
				state->BeginPerfEvent("Precipitation Mask");

				if (auto precipObject = GetActivePrecipitationObject(precip)) {
					precip->SetupMask();
					if (auto* rain = GetRainEmitter(precipObject))
						precip->RenderMask(rain);
				}

				state->EndPerfEvent();
			}

			{
				state->BeginPerfEvent("Skylighting Mask");

				const bool forceOcclusionRefresh = queuedResetSkylighting;
				if (queuedResetSkylighting)
					ResetSkylighting();

				const uint occlusionUpdateInterval = GetOcclusionUpdateInterval(settings);
				const bool shouldUpdateOcclusion = ShouldRunPeriodicUpdate(occlusionUpdateFrameCounter, occlusionUpdateInterval, forceOcclusionRefresh);

				if (!shouldUpdateOcclusion) {
					state->EndPerfEvent();
					return;
				}

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
				PrecipitationShaderCubeSize = occlusionDistance;

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
				_computeProjection(precip, precip->occlusionData.camera);
				precip->SetupMask();

				RainEmitterProjectionCapture rainCapture{};
				{
					TracyD3D11Zone(state->tracyCtx, "Skylighting - Render Height Map");
					precip->RenderMask(reinterpret_cast<RE::BSParticleShaderRainEmitter*>(&rainCapture));
				}
				inOcclusion = false;

				OcclusionDir = -float4{ PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z, 0 };
				occlusionSHBasis4Pi = EvaluateDirectionalSHBasis4Pi(float3{ OcclusionDir.x, OcclusionDir.y, OcclusionDir.z });
				OcclusionTransform = reinterpret_cast<RE::BSParticleShaderRainEmitter*>(&rainCapture)->occlusionProjection;

				PrecipitationShaderCubeSize = originalPrecipitationShaderCubeSize;
				precip->lastCubeSize = originaLastCubeSize;

				PrecipitationShaderDirection = originalParticleShaderDirection;

				precipitation = precipitationCopy;

				_computeProjection(precip, precip->occlusionData.camera);

				state->EndPerfEvent();
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
