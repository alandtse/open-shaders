#include "NativeLighting.h"
#include "Features/NativeComposite/NativeComposite.h"
#include "Features/NativeFeatures.h"
#include "Features/NativePBR/NativeSnowSampling.h"
#include "Features/ReverseZ.h"
#include "Globals.h"
#include "GpuPass.h"
#include "NativeLightingRoot.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"
#include <bit>
#include <cmath>
#include <directx/d3dx12.h>
#include <fstream>
#include <span>
#include <unordered_map>

namespace
{
	constexpr float kSnowProbeGrid = 256.0f;
	constexpr float kSnowNearbyCoverage = 0.5f;

	std::vector<uint8_t> ReadNativeSnowTerrain(const DX12Native::TopLevel& a_scene)
	{
		std::vector<uint8_t> coverage(a_scene.instances.size(), 0);
		auto* tes = globals::game::tes;
		if (!tes || Util::IsInterior())
			return coverage;
		std::unordered_map<uint64_t, bool> samples;
		const auto isSnow = [&](int32_t x, int32_t y, float z) {
			const uint64_t key = (uint64_t(uint32_t(x)) << 32u) | uint32_t(y);
			if (auto it = samples.find(key); it != samples.end())
				return it->second;
			RE::NiPoint3 point((float(x) + 0.5f) * kSnowProbeGrid,
				(float(y) + 0.5f) * kSnowProbeGrid, z);
			float landZ = 0.0f;
			bool snowy = false;
			if (tes->GetLandHeight(point, landZ) && std::isfinite(landZ)) {
				point.z = landZ;
				const auto material = tes->GetLandMaterialType(point);
				snowy = material == RE::MATERIAL_ID::kSnow || material == RE::MATERIAL_ID::kSnowStairs;
			}
			samples.emplace(key, snowy);
			return snowy;
		};
		for (size_t i = 0; i < a_scene.instances.size(); ++i) {
			const auto& instance = a_scene.instances[i];
			if (!instance.material.projectedSnowCandidate ||
				!std::isfinite(instance.bound.x) || !std::isfinite(instance.bound.y) ||
				!std::isfinite(instance.bound.z) || std::abs(instance.bound.x) > 1e7f ||
				std::abs(instance.bound.y) > 1e7f)
				continue;
			const auto x = int32_t(std::floor(instance.bound.x / kSnowProbeGrid));
			const auto y = int32_t(std::floor(instance.bound.y / kSnowProbeGrid));
			if (isSnow(x, y, instance.bound.z))
				coverage[i] = 255;
			else if (isSnow(x - 1, y, instance.bound.z) || isSnow(x + 1, y, instance.bound.z) ||
				isSnow(x, y - 1, instance.bound.z) || isSnow(x, y + 1, instance.bound.z))
				coverage[i] = uint8_t(kSnowNearbyCoverage * 255.0f + 0.5f);
		}
		return coverage;
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeLightingFeature::Settings, OffloadStaticShadows, OffloadLLFLights, PreserveMatchedRasterShadows, PreserveMatchedRasterLights, FadeShadowsNearReach, OffloadedShadowIntensity, Enabled, Diffuse, Specular, EarlyShadowCull, Debug, CompareCulling, DiffuseIntensity, SpecularIntensity, BudgetMB, PersistLab, DirectStrength, EmissionStrength, PointLightRadiusMultiplier, PointLightFalloffFraction, PointLightFalloffExponent, ShadowStrength, EmitterRadiusMultiplier, NormalInfluence, SelectedDirectLight, StochasticShadows, UseAllPreparedLights, TraceDensityMode, CharacterCasterOcclusion, CharacterReceiverProbe)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeLightingFeature::ValidityLab, DepthAgreement, DiagnosticDepth, AbsoluteTolerance, RelativeTolerance, MaterialPolicy, RequireMaskAuthority, AssumeMaskAuthoritative, ObserveMask, RunWithoutLighting, ReferenceVisibility, ReferencePrimaryTraversal, MaskThreshold, MaskDistanceTolerance)

void NativeLightingFeature::DrawSettings()
{
	std::scoped_lock lock(settingsMutex);
	int mode = 0;
	ImGui::Combo("Mode selection", &mode, "DX12 lighting/shadow offloading\0");
	int traceDensity = int(settings.TraceDensityMode);
	if (ImGui::Combo("Sampling density", &traceDensity, "Full\0Two-thirds\0Half\0One-third\0Quarter\0One-sixth\0One-ninth\0Four-ninths\0")) {
		settings.TraceDensityMode = uint32_t(traceDensity);
		++validityRevision;
	}
	if (ImGui::Checkbox("Offload static shadows", &settings.OffloadStaticShadows))
		++validityRevision;
	if (ImGui::SliderFloat("Offloaded shadow intensity", &settings.OffloadedShadowIntensity, 0.0f, 4.0f, "%.2fx"))
		++validityRevision;
	ImGui::BeginDisabled(!settings.OffloadStaticShadows);
	if (ImGui::Checkbox("Preserve matched raster shadows", &settings.PreserveMatchedRasterShadows))
		++validityRevision;
	if (ImGui::Checkbox("Fade shadows near light reach", &settings.FadeShadowsNearReach))
		++validityRevision;
	ImGui::EndDisabled();
	if (ImGui::Checkbox("Offload LLF lights", &settings.OffloadLLFLights))
		++validityRevision;
	ImGui::BeginDisabled(!settings.OffloadLLFLights);
	if (ImGui::Checkbox("Preserve matched raster lighting", &settings.PreserveMatchedRasterLights))
		++validityRevision;
	ImGui::EndDisabled();
	ImGui::TextDisabled("Disabling also removes these LLF lights from raster-only receivers (including characters).");
}

NativeLightingFeature::OffloadSettings NativeLightingFeature::GetOffloadSettings() const
{
	std::scoped_lock lock(settingsMutex);
	return { settings.OffloadStaticShadows, settings.OffloadLLFLights, settings.PreserveMatchedRasterLights,
		settings.PreserveMatchedRasterShadows, settings.FadeShadowsNearReach,
		settings.OffloadedShadowIntensity, settings.TraceDensityMode, validityRevision };
}

void NativeLightingFeature::LoadSettings(json& a_json)
{
	auto selected = a_json.get<Settings>();
	selected.DiffuseIntensity = std::isfinite(selected.DiffuseIntensity) ? std::clamp(selected.DiffuseIntensity, 0.0f, 8.0f) : 1.0f;
	selected.SpecularIntensity = std::isfinite(selected.SpecularIntensity) ? std::clamp(selected.SpecularIntensity, 0.0f, 8.0f) : 1.0f;
	const auto sanitize = [](float v, float fallback, float low, float high) { return std::isfinite(v) ? std::clamp(v, low, high) : fallback; };
	selected.DirectStrength = sanitize(selected.DirectStrength, Settings{}.DirectStrength, 0.0f, 8.0f);
	selected.EmissionStrength = sanitize(selected.EmissionStrength, Settings{}.EmissionStrength, 0.0f, 8.0f);
	selected.PointLightRadiusMultiplier = sanitize(selected.PointLightRadiusMultiplier, Settings{}.PointLightRadiusMultiplier, 0.1f, 64.0f);
	selected.PointLightFalloffFraction = sanitize(selected.PointLightFalloffFraction, 0.1f, 0.02f, 1.0f);
	selected.PointLightFalloffExponent = sanitize(selected.PointLightFalloffExponent, 1.0f, 0.25f, 4.0f);
	selected.ShadowStrength = sanitize(selected.ShadowStrength, 1.0f, 0.0f, 1.0f);
	selected.EmitterRadiusMultiplier = sanitize(selected.EmitterRadiusMultiplier, 1.0f, 0.0f, 8.0f);
	selected.NormalInfluence = std::min(selected.NormalInfluence, 2u);
	selected.TraceDensityMode = std::min(selected.TraceDensityMode, 7u);
	selected.OffloadedShadowIntensity = sanitize(selected.OffloadedShadowIntensity, 1.0f, 0.0f, 4.0f);
	selected.ForcePositiveValidity = false;
	selected.BudgetMB = 0;  // Legacy manual cap is ignored.
	std::scoped_lock lock(settingsMutex);
	settings = selected;
	lab = selected.PersistLab && a_json.contains("ValidityLab") ? a_json.at("ValidityLab").get<ValidityLab>() : ValidityLab{};
	lab.AbsoluteTolerance = std::isfinite(lab.AbsoluteTolerance) ? std::clamp(lab.AbsoluteTolerance, 0.0f, 10000.0f) : 5.0f;
	lab.RelativeTolerance = std::isfinite(lab.RelativeTolerance) ? std::clamp(lab.RelativeTolerance, 0.0f, 1.0f) : 0.00001f;
	lab.MaterialPolicy = std::min(lab.MaterialPolicy, 2u);
	lab.MaskThreshold = std::isfinite(lab.MaskThreshold) ? std::clamp(lab.MaskThreshold, 0.0f, 1.0f) : 0.0f;
	lab.MaskDistanceTolerance = std::isfinite(lab.MaskDistanceTolerance) ? std::clamp(lab.MaskDistanceTolerance, 0.0f, 10000.0f) : 2.0f;
	++validityRevision;
}
void NativeLightingFeature::SaveSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	a_json = settings;
	if (settings.PersistLab)
		a_json["ValidityLab"] = lab;
}
void NativeLightingFeature::RestoreDefaultSettings()
{
	std::scoped_lock lock(settingsMutex);
	settings = {};
	lab = {};
	++validityRevision;
}
void NativeLightingFeature::OnSceneTransitionReset(bool a_open)
{
	transitionOpen = a_open;
	resetRequested = true;
}

NativeLightingFeature::~NativeLightingFeature()
{
	current.reset();
	const auto done = completion ? completion->GetCompletedValue() : 0;
	for (auto& frame : frames) {
		if (!frame)
			continue;
		const bool consumerPending = std::ranges::any_of(frame->consumers, [](const auto& item) {
			const auto value = item.first->GetCompletedValue();
			return value == UINT64_MAX || value < item.second;
		});
		if (done == UINT64_MAX || frame->readyValue == UINT64_MAX || frame->readyValue > done || consumerPending)
			(void)new std::shared_ptr<Contribution>(std::move(frame));
	}
}

void NativeLightingFeature::Reset()
{
	current.reset();
	snowTerrainScene.reset();
	snowTerrainCoverage.clear();
	active = false;
	status.clear();
	failed = false;
}

void NativeLightingFeature::Retire()
{
	if (!completion)
		return;
	const auto done = completion->GetCompletedValue();
	if (done == UINT64_MAX) {
		status = "Native lighting device removed";
		failed = true;
		return;
	}
	for (auto& frame : frames) {
		if (!frame || frame->countersRead || !frame->readyValue || frame->readyValue == UINT64_MAX || frame->readyValue > done)
			continue;
		void* mapped = nullptr;
		D3D12_RANGE read{ 0, sizeof(uint32_t) * totals.size() };
		if (FAILED(frame->counterReadback->Map(0, &read, &mapped))) {
			++counterReadFailures;
			status = "Native lighting counter readback unavailable";
			continue;
		}
		const auto* counts = static_cast<const uint32_t*>(mapped);
		for (size_t i = 0; i < totals.size(); ++i) totals[i] += counts[i];
		const uint64_t gridPixels = uint64_t(frame->width) * frame->height;
		// Counter 0 runs for all selected pixels, before any primary ray query.
		// Derive omitted coverage without a new contended GPU atomic counter.
		skippedPixels += gridPixels - std::min<uint64_t>(gridPixels, counts[0]);
		D3D12_RANGE noWrite{ 0, 0 };
		frame->counterReadback->Unmap(0, &noWrite);
		frame->countersRead = true;
		frame->scene = {};
		frame->rayMaterials.reset();
		++completions;
	}
}

std::shared_ptr<const NativeLightingFeature::Contribution> NativeLightingFeature::GetContribution(uint32_t a_frame) const
{
	return loaded && active && !transitionOpen && current && current->frame == a_frame ? current : nullptr;
}
bool NativeLightingFeature::RetainContribution(const std::shared_ptr<const Contribution>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
{
	if (!a_frame || !a_fence || !a_value || a_value == UINT64_MAX)
		return false;
	for (const auto& frame : frames) {
		if (frame.get() != a_frame.get())
			continue;
		winrt::com_ptr<ID3D12Fence> retained;
		retained.copy_from(a_fence);
		frame->consumers.emplace_back(std::move(retained), a_value);
		return true;
	}
	return false;
}

void NativeLightingFeature::PublishDiagnostics()
{
	const auto offload = GetOffloadSettings();
	json snapshot{ { "status", status }, { "submissions", submissions }, { "gpuCompletions", completions },
		{ "frameSlots", frames.size() }, { "busyDeferrals", busyDeferrals },
		{ "dxgiAvailable", videoBudget.last.dxgiAvailable }, { "effectiveFrameBudgetBytes", videoBudget.last.effectiveBytes },
		{ "dxgiLocalBudgetBytes", videoBudget.last.localBudget }, { "dxgiLocalUsageBytes", videoBudget.last.localUsage }, { "counterReadFailures", counterReadFailures },
		{ "candidatePixels", totals[0] }, { "skippedPixels", skippedPixels },
		{ "traceDensityMode", current ? current->traceDensityMode : 0u },
		{ "signalWidth", current ? current->width : 0u }, { "signalHeight", current ? current->height : 0u },
		{ "maskCulled", totals[1] }, { "maskUnavailable", totals[2] },
		{ "primaryQueries", totals[3] }, { "primaryHits", totals[4] }, { "primaryMisses", totals[5] },
		{ "materials", totals[6] }, { "shadowQueries", totals[7] }, { "lights", totals[8] },
		{ "nativePbrEvaluated", totals[28] }, { "nativePbrCandidates", totals[29] },
		{ "nativePbrForcedTextures", totals[30] },
		{ "forcePositiveValidity", current && current->forcePositiveValidity },
		{ "validDiffuse", totals[9] }, { "unsupported", totals[10] }, { "validSpecular", totals[11] },
		{ "currentMaskAuthoritative", false }, { "comparisonFrames", comparisonFrames }, { "visualConsumptionVerified", false } };
	snapshot["offloading"] = { { "staticShadows", offload.staticShadows }, { "llfLights", offload.llfLights },
		{ "preserveMatchedRasterShadows", offload.preserveMatchedRasterShadows }, { "fadeShadowsNearReach", offload.fadeShadowsNearReach },
		{ "preserveMatchedRasterLights", offload.preserveMatchedRasterLights },
		{ "shadowIntensity", offload.shadowIntensity }, { "samplingDensity", offload.traceDensity } };
	constexpr std::array reasonNames{ "invalidCamera", "noPrimaryHit", "depthMismatch", "unsupportedMaterial", "materialEvaluationFailure", "noEligibleLights", "invalidVisibility", "nonfiniteLighting", "authoritativeMaskCull", "successfulEvaluation" };
	for (size_t i = 0; i < reasonNames.size(); ++i) snapshot["originalRejections"][reasonNames[i]] = totals[12 + i];
	snapshot["recoveredPixels"] = totals[22];
	snapshot["experimentalMaskCulls"] = totals[23];
	snapshot["recoveredDepthMismatch"] = totals[24];
	snapshot["recoveredMaterialFailure"] = totals[25];
	snapshot["recoveredVisibility"] = totals[26];
	snapshot["receiverIdentityAvailable"] = false;
	snapshot["maskAvailable"] = current && current->maskAvailable;
	snapshot["maskExperimental"] = current && current->maskExperimental;
	snapshot["characterCasterOcclusion"] = current && bool(current->characterCasters);
	snapshot["characterCasterFrame"] = current && current->characterCasters ?
	                                       current->characterCasters->poseFrame :
	                                       0u;
	snapshot["characterReceiverProbeEnabled"] =
		current && current->characterProbeEnabled;
	snapshot["characterReceiverRasterMatches"] = totals[27];
	std::scoped_lock lock(diagnosticsMutex);
	diagnostics = std::move(snapshot);
}
json NativeLightingFeature::GetDiagnostics()
{
	std::scoped_lock lock(diagnosticsMutex);
	return diagnostics;
}

void NativeLightingFeature::RegisterUxActions()
{
	FEATURE_COMMAND("configure", "DX12 lighting/shadow offloading: staticShadows, llfLights, preserveMatchedRasterShadows, preserveMatchedRasterLights, fadeShadowsNearReach (bool), shadowIntensity (0..4), samplingDensity (0 Full, 1 Two-thirds, 2 Half, 3 One-third, 4 Quarter, 5 One-sixth, 6 One-ninth, 7 Four-ninths).", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativeLightingFeature*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		if (args.contains("staticShadows"))
			feature.settings.OffloadStaticShadows = args.at("staticShadows").get<bool>();
		if (args.contains("llfLights"))
			feature.settings.OffloadLLFLights = args.at("llfLights").get<bool>();
		if (args.contains("preserveMatchedRasterShadows"))
			feature.settings.PreserveMatchedRasterShadows = args.at("preserveMatchedRasterShadows").get<bool>();
		if (args.contains("preserveMatchedRasterLights"))
			feature.settings.PreserveMatchedRasterLights = args.at("preserveMatchedRasterLights").get<bool>();
		if (args.contains("fadeShadowsNearReach"))
			feature.settings.FadeShadowsNearReach = args.at("fadeShadowsNearReach").get<bool>();
		if (args.contains("shadowIntensity")) {
			const float value = args.at("shadowIntensity").get<float>();
			if (std::isfinite(value))
				feature.settings.OffloadedShadowIntensity = std::clamp(value, 0.0f, 4.0f);
		}
		if (args.contains("samplingDensity"))
			feature.settings.TraceDensityMode = std::min(args.at("samplingDensity").get<uint32_t>(), 7u);
		++feature.validityRevision;
	});
	FEATURE_COMMAND("validityLab", "Set runtime validity controls: depthAgreement, diagnosticDepth, absoluteTolerance, relativeTolerance, materialPolicy (0 strict, 1 neutral, 2 geometry), requireMaskAuthority, assumeMaskAuthoritative, observeMask, runWithoutLighting, referenceVisibility, referencePrimaryTraversal, maskThreshold, maskDistanceTolerance, persistLab.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativeLightingFeature*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		auto& lab = feature.lab;
		if (args.contains("depthAgreement"))
			lab.DepthAgreement = args.at("depthAgreement").get<bool>();
		if (args.contains("persistLab"))
			feature.settings.PersistLab = args.at("persistLab").get<bool>();
		if (args.contains("diagnosticDepth"))
			lab.DiagnosticDepth = args.at("diagnosticDepth").get<bool>();
		if (args.contains("absoluteTolerance")) {
			const auto v = args.at("absoluteTolerance").get<float>();
			if (std::isfinite(v))
				lab.AbsoluteTolerance = std::clamp(v, 0.0f, 10000.0f);
		}
		if (args.contains("relativeTolerance")) {
			const auto v = args.at("relativeTolerance").get<float>();
			if (std::isfinite(v))
				lab.RelativeTolerance = std::clamp(v, 0.0f, 1.0f);
		}
		if (args.contains("materialPolicy"))
			lab.MaterialPolicy = std::min(args.at("materialPolicy").get<uint32_t>(), 2u);
		if (args.contains("requireMaskAuthority"))
			lab.RequireMaskAuthority = args.at("requireMaskAuthority").get<bool>();
		if (args.contains("assumeMaskAuthoritative"))
			lab.AssumeMaskAuthoritative = args.at("assumeMaskAuthoritative").get<bool>();
		if (args.contains("observeMask"))
			lab.ObserveMask = args.at("observeMask").get<bool>();
		if (args.contains("runWithoutLighting"))
			lab.RunWithoutLighting = args.at("runWithoutLighting").get<bool>();
		if (args.contains("referenceVisibility"))
			lab.ReferenceVisibility = args.at("referenceVisibility").get<bool>();
		if (args.contains("referencePrimaryTraversal"))
			lab.ReferencePrimaryTraversal = args.at("referencePrimaryTraversal").get<bool>();
		if (args.contains("maskThreshold")) {
			const auto v = args.at("maskThreshold").get<float>();
			if (std::isfinite(v))
				lab.MaskThreshold = std::clamp(v, 0.0f, 1.0f);
		}
		if (args.contains("maskDistanceTolerance")) {
			const auto v = args.at("maskDistanceTolerance").get<float>();
			if (std::isfinite(v))
				lab.MaskDistanceTolerance = std::clamp(v, 0.0f, 10000.0f);
		}
		++feature.validityRevision;
	});
	FEATURE_QUERY("validityLabStatus", "Read runtime validity settings, persistence choice and policy revision.", ([](const Feature* self, const json&) -> json {
		auto& feature = *const_cast<NativeLightingFeature*>(static_cast<const NativeLightingFeature*>(self));
		std::scoped_lock lock(feature.settingsMutex);
		return { { "settings", feature.lab }, { "persistLab", feature.settings.PersistLab }, { "revision", feature.validityRevision }, { "receiverIdentityAvailable", false } };
	}));
}

bool NativeLightingFeature::Initialize(const GraphicsBackend& a_backend)
{
	if (failed)
		return false;
	if (completion && (backend.d3d12Device.get() != a_backend.d3d12Device.get() || backend.commandQueue.get() != a_backend.commandQueue.get())) {
		status = "Native lighting graphics backend changed";
		failed = true;
		return false;
	}
	if (pipeline)
		return true;
	try {
		backend = a_backend;
		if (!backend.d3d12Device || !backend.commandQueue)
			throw std::runtime_error("Native lighting backend unavailable");
		const auto* shaderPath = globals::features::reverseZ.IsActive() ? "Data/Shaders/NativeLighting/DirectReverseZ.dxil" : "Data/Shaders/NativeLighting/Direct.dxil";
		std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
		if (!file)
			throw std::runtime_error("Native lighting shader unavailable");
		const auto size = file.tellg();
		if (size <= 0 || size > 16 * 1024 * 1024)
			throw std::runtime_error("Native lighting shader size invalid");
		std::vector<char> shader(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(shader.data(), size))
			throw std::runtime_error("Native lighting shader read failed");
		static_assert(NativeLighting::kMaximumInstances == DX12Native::ShadowBindings::kMaximumInstances && NativeLighting::kMaximumTexturePools == DX12Native::kMaximumTexturePools);
		DX::ThrowIfFailed(NativeLighting::CreateRootSignature(backend.d3d12Device.get(), root.put()));
		root->SetName(L"NativeLighting::Root");
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = root.get();
		pso.CS = { shader.data(), shader.size() };
		DX::ThrowIfFailed(backend.d3d12Device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pipeline.put())));
		pipeline->SetName(L"NativeLighting::DirectPipeline");
		if (!completion) {
			DX::ThrowIfFailed(backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
			completion->SetName(L"NativeLighting::Completion");
		}
		return true;
	} catch (const std::exception& error) {
		status = error.what();
		failed = true;
		return false;
	}
}

void NativeLightingFeature::OnOpaqueRasterReady(const OpaqueRasterInputs&)
{
	CS_GPU_PASS("NativeLighting::Direct");
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	Retire();
	if (reloadRequested.exchange(false)) {
		Reset();
		pipeline = nullptr;
		root = nullptr;
	}
	if (resetRequested.exchange(false))
		Reset();
	Settings selected;
	ValidityLab selectedLab;
	uint64_t revision;
	{
		std::scoped_lock lock(settingsMutex);
		selected = settings;
		selectedLab = lab;
		revision = validityRevision;
	}
	selected.Enabled = selected.OffloadLLFLights;
	selected.Diffuse = true;
	selected.Specular = false;
	selected.DiffuseIntensity = 1.0f;
	selected.SpecularIntensity = 0.0f;
	selected.EmissionStrength = 0.0f;
	selected.DirectStrength = 1.0f;
	selected.PointLightRadiusMultiplier = 1.0f;
	selected.PointLightFalloffFraction = 0.1f;
	selected.PointLightFalloffExponent = 1.0f;
	selected.NormalInfluence = 2u;
	selected.StochasticShadows = false;
	selected.EarlyShadowCull = false;
	selected.UseAllPreparedLights = true;
	selected.ForcePositiveValidity = false;
	selected.CharacterCasterOcclusion = false;
	selected.CharacterReceiverProbe = false;
	selected.Debug = false;
	selected.CompareCulling = false;
	if (!selected.Enabled || (!selected.Diffuse && !selected.Specular && selectedLab.MaterialPolicy != 2 && !selectedLab.RunWithoutLighting && !selected.ForcePositiveValidity) || transitionOpen || globals::game::isVR) {
		current.reset();
		active = false;
		status = "Native direct lighting disabled or unavailable";
		return;
	}
	if (!globals::state || !globals::state->inWorld || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	if (!globals::game::graphicsState) {
		current.reset();
		active = false;
		status = "Current frame unavailable";
		return;
	}
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (current && current->frame == frame)
		return;
	current.reset();
	active = false;
	auto& owner = globals::features::dx12NativeScene;
	auto scene = owner.GetSceneFrame(frame);
	auto graphics = owner.GetGraphicsBackend();
	if (!scene || !graphics || !scene->raster || !scene->raster->cameraValid || !scene->prepared || !scene->prepared->materials) {
		status = "Waiting for current scene, raster depth, camera or material bindings";
		return;
	}
	const auto nativeLights = owner.GetNativeLLFLights(frame);
	const auto availableLights = nativeLights ? size_t(nativeLights->count) : scene->prepared->cell->nativeLights.size();
	if ((!nativeLights && selected.UseAllPreparedLights && availableLights > DX12Native::kMaximumNativePointLights) ||
		(!selected.UseAllPreparedLights && selected.SelectedDirectLight >= availableLights && !nativeLights && !selected.ForcePositiveValidity) ||
		(availableLights == 0 && !nativeLights && !selectedLab.RunWithoutLighting && selectedLab.MaterialPolicy != 2 && !selected.ForcePositiveValidity)) {
		status = "Prepared point-light selection unavailable or exceeds 64";
		return;
	}
	if (!Initialize(*graphics))
		return;
	Dispatch(std::move(*scene), selected, selectedLab, revision);
}

void NativeLightingFeature::Dispatch(DX12NativeScene::SceneFrame a_scene, const Settings& a_settings, const ValidityLab& a_lab, uint64_t a_revision)
{
	const auto completed = completion->GetCompletedValue();
	if (completed == UINT64_MAX) {
		status = "Native lighting device removed";
		failed = true;
		return;
	}
	auto available = std::find_if(frames.begin(), frames.end(), [&](const auto& frame) {
		if (!frame)
			return true;
		if (frame.use_count() != 1 || frame->readyValue == UINT64_MAX || frame->readyValue > completed || (frame->readyValue && !frame->countersRead))
			return false;
		return std::ranges::all_of(frame->consumers, [](const auto& consumer) {
			const auto value = consumer.first->GetCompletedValue();
			return value != UINT64_MAX && value >= consumer.second;
		});
	});
	if (available == frames.end()) {
		++busyDeferrals;
		status = "Native lighting outputs are still in use";
		return;
	}
	try {
		const auto size = Util::ConvertToDynamic(globals::state->screenSize);
		const uint32_t width = uint32_t(size.x), height = uint32_t(size.y);
		if (!width || !height || width > a_scene.raster->width || height > a_scene.raster->height || width > 16384 || height > 16384)
			throw std::runtime_error("Native lighting dimensions unavailable");
		constexpr std::array formats{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_UINT,
			DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT };
		uint64_t trackedBytes = 0;
		for (const auto& frame : frames)
			if (frame) {
				if (trackedBytes > UINT64_MAX - frame->bytes)
					throw std::runtime_error("Native lighting pool accounting overflow");
				trackedBytes += frame->bytes;
			}
		const uint64_t budget = videoBudget.Query(
											   backend.d3d12Device.get(), trackedBytes, 0)
		                            .effectiveBytes;
		uint64_t bytes = 2 * 1024 * 1024;
		const auto rayTable = CD3DX12_RESOURCE_DESC::Buffer(NativeLighting::kMaximumInstances * sizeof(NativePBR::GpuDrawBinding));
		const auto rayBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &rayTable).SizeInBytes;
		if (rayBytes == UINT64_MAX || rayBytes > budget || bytes > budget - rayBytes)
			throw std::runtime_error("Native lighting PBR binding budget exceeded");
		bytes += rayBytes;
		for (size_t i = 0; i < formats.size(); ++i) {
			if (!a_settings.CompareCulling && (i == 5 || i == 6))
				continue;
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(formats[i], width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			const auto allocation = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
			if (allocation == UINT64_MAX || allocation > budget || bytes > budget - allocation)
				throw std::runtime_error("Native lighting output budget exceeded");
			bytes += allocation;
		}
		for (auto it = frames.begin(); it != frames.end(); ++it)
			if (it != available && *it) {
				if ((*it)->bytes > budget || bytes > budget - (*it)->bytes)
					throw std::runtime_error("Native lighting retained frames exceed budget");
				bytes += (*it)->bytes;
			}
		auto& slot = *available;
		if (!slot || slot->width != width || slot->height != height || slot->comparisonEnabled != a_settings.CompareCulling) {
			slot.reset();
			auto created = std::make_shared<Contribution>();
			created->width = width;
			created->height = height;
			created->comparisonEnabled = a_settings.CompareCulling;
			created->bytes = bytes;
			for (auto it = frames.begin(); it != frames.end(); ++it)
				if (it != available && *it)
					created->bytes -= (*it)->bytes;
			const auto make = [&](const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, const wchar_t* name) {
				winrt::com_ptr<ID3D12Resource> resource;
				const auto heap = CD3DX12_HEAP_PROPERTIES(type);
				DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(resource.put())));
				resource->SetName(name);
				return resource;
			};
			constexpr std::array names{ L"NativeLighting::RawDiffuse", L"NativeLighting::RawSpecular", L"NativeLighting::Validity", L"NativeLighting::ReceiverGuide", L"NativeLighting::Debug", L"NativeLighting::ComparisonDiffuse", L"NativeLighting::ComparisonSpecular", L"NativeLighting::EffectiveValidity", L"NativeLighting::RejectionReason", L"NativeLighting::LabGuide" };
			for (size_t i = 0; i < formats.size(); ++i) {
				if (!a_settings.CompareCulling && (i == 5 || i == 6))
					continue;
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(formats[i], width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				created->textures[i] = make(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, names[i]);
			}
			const auto counters = CD3DX12_RESOURCE_DESC::Buffer(totals.size() * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			const auto plain = CD3DX12_RESOURCE_DESC::Buffer(totals.size() * sizeof(uint32_t));
			created->counters = make(counters, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"NativeLighting::Counters");
			created->counterReset = make(plain, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"NativeLighting::CounterReset");
			created->counterReadback = make(plain, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, L"NativeLighting::CounterReadback");
			created->lights = make(CD3DX12_RESOURCE_DESC::Buffer(DX12Native::kMaximumNativePointLights * 48), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"NativeLighting::Lights");
			created->rayBindings = make(rayTable, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"NativeLighting::RayMaterialBindings");
			// 352-byte Trace ABI rounds to 512 for the D3D12 CBV 256-byte alignment.
			created->constants = make(CD3DX12_RESOURCE_DESC::Buffer(512), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"NativeLighting::Constants");
			void* zeros = nullptr;
			const D3D12_RANGE noRead{ 0, 0 };
			DX::ThrowIfFailed(created->counterReset->Map(0, &noRead, &zeros));
			std::memset(zeros, 0, totals.size() * sizeof(uint32_t));
			created->counterReset->Unmap(0, nullptr);
			constexpr uint32_t kDescriptors = 13 + 2 * DX12Native::ShadowBindings::kMaximumInstances + DX12Native::kMaximumTexturePools + NativePBR::kGpuTextureDescriptorCapacity;
			const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			DX::ThrowIfFailed(backend.d3d12Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(created->heap.put())));
			created->heap->SetName(L"NativeLighting::Descriptors");
			DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(created->allocator.put())));
			slot = std::move(created);
		}
		slot->scene = std::move(a_scene);
		slot->rayMaterials = globals::features::nativePBR.GetRayMaterialBindings();
		if (slot->rayMaterials &&
			(slot->rayMaterials->eligible == 0 ||
			slot->rayMaterials->prepared->geometry != slot->scene.prepared->acceleration ||
			slot->rayMaterials->prepared->sceneResources != slot->scene.prepared->materials ||
			slot->rayMaterials->instances.size() != slot->scene.prepared->acceleration->instances.size() ||
			!slot->rayMaterials->materials->records || !slot->rayMaterials->materials->textureDescriptors ||
			slot->rayMaterials->materials->device.get() != backend.d3d12Device.get()))
			slot->rayMaterials.reset();
		// Independent Native Lighting visibility never assumes the separate
		// shadow-mask feature is enabled. Retain the exact TLAS until this
		// contribution's GPU fence and downstream consumers complete.
		slot->characterCasters =
			(a_settings.CharacterCasterOcclusion ||
				a_settings.CharacterReceiverProbe) ?
				globals::features::dx12NativeScene.GetCharacterCasterForVisibility(
					slot->scene.frame) :
				nullptr;
		slot->characterProbeEnabled = a_settings.CharacterReceiverProbe &&
		                              bool(slot->characterCasters);
		if (!a_settings.EarlyShadowCull || (!a_lab.ObserveMask && !a_lab.AssumeMaskAuthoritative))
			slot->scene.shadow.reset();
		if (slot->scene.shadow && (!slot->scene.shadow->mask || !slot->scene.shadow->mask->resource ||
									  !slot->scene.shadow->readyFence || !slot->scene.shadow->readyValue || slot->scene.shadow->readyValue == UINT64_MAX ||
									  slot->scene.shadow->frame != slot->scene.frame || slot->scene.shadow->width != width || slot->scene.shadow->height != height ||
									  slot->scene.shadow->retainedBackend.d3d12Device.get() != backend.d3d12Device.get() ||
									  slot->scene.shadow->scene != slot->scene.prepared->acceleration))
			slot->scene.shadow.reset();
		slot->sceneRevision = slot->scene.prepared->revision;
		slot->validityRevision = a_revision;
		slot->geometryOnly = a_lab.MaterialPolicy == 2;
		slot->forcePositiveValidity = a_settings.ForcePositiveValidity;
		slot->maskAvailable = static_cast<bool>(slot->scene.shadow);
		slot->maskExperimental = slot->maskAvailable && a_settings.EarlyShadowCull && !a_lab.RequireMaskAuthority && a_lab.AssumeMaskAuthoritative;
		slot->frame = slot->scene.frame;
		slot->enabledLobes = (a_settings.Diffuse ? 1u : 0u) | (a_settings.Specular ? 2u : 0u);
		slot->diffuseIntensity = a_settings.DiffuseIntensity;
		slot->specularIntensity = a_settings.SpecularIntensity;
		slot->traceDensityMode = a_settings.TraceDensityMode;
		slot->root = root;
		slot->pipeline = pipeline;
		slot->backend = backend;
		slot->readyFence = completion;
		slot->consumers.clear();
		slot->readyValue = 0;
		slot->countersRead = true;
		const auto& scene = *slot->scene.prepared;
		const auto& camera = slot->scene.raster->camera;
		std::array<DX12Native::NativeDirectPointLight, DX12Native::kMaximumNativePointLights> lights{};
		const auto nativeLights = globals::features::dx12NativeScene.GetNativeLLFLights(slot->scene.frame);
		const std::span<const DX12Native::PreparedPointLight> lightSource = nativeLights ?
		                                                                        std::span<const DX12Native::PreparedPointLight>(nativeLights->lights.data(), nativeLights->count) :
		                                                                        std::span<const DX12Native::PreparedPointLight>(scene.cell->nativeLights);
		const uint32_t selectedCount = a_settings.UseAllPreparedLights ? uint32_t(std::min<size_t>(lightSource.size(), lights.size())) :
		                                                                 (lightSource.empty() ? 0u : 1u);
		for (uint32_t i = 0; i < selectedCount; ++i) {
			const auto& light = lightSource[a_settings.UseAllPreparedLights ? i :
																			  std::min<size_t>(a_settings.SelectedDirectLight, lightSource.size() - 1)];
			lights[i] = { { light.position.x, light.position.y, light.position.z, light.radius },
				{ light.radiance.x, light.radiance.y, light.radiance.z, light.sourceRadius },
				{ light.supported ? 1u : 0u, light.falloffMode, std::bit_cast<uint32_t>(light.fadeZone), 0 } };
		}
		struct Constants
		{
			uint32_t width, height, instances, lights;
			float3 origin;
			float minimum;
			float diffuse, specular;
			uint32_t lobes, maskAuthoritative;
			uint32_t maskRequested, debug, comparison, reserved;
			DirectX::SimpleMath::Matrix inverseProjection, inverseView;
			float absoluteTolerance, relativeTolerance, maskThreshold, maskDistanceTolerance;
			uint32_t depthMode, materialPolicy, maskAvailable, maskExperimental;
			uint32_t referenceVisibility, referencePrimaryTraversal, frameIndex, traceDensityMode;
			float4 lightTuning;   // reach, cutoff width, exponent, common direct strength
			float4 shadowTuning;  // shadow strength, emitter scale, stochastic enabled, emission
			uint32_t nativePbrReady, nativePbrPadding[2], nativePbrMaterialOptions;
			float4 snowTuning, snowSurface, snowLumps, snowDetailControls;
		};
		static_assert(sizeof(Constants) == 352);
		const auto snowSettings = globals::features::nativePBR.GetSnowSettings();
		const float snowRayCells = NativePBR::SnowRayCellFrequency(width, height,
			a_settings.TraceDensityMode, snowSettings.noiseRaysPerPatch);
		const Constants constants{ width, height, uint32_t(scene.acceleration->instances.size()), selectedCount,
			{ camera.CameraPosAdjust.x + camera.CameraViewInverse._14, camera.CameraPosAdjust.y + camera.CameraViewInverse._24, camera.CameraPosAdjust.z + camera.CameraViewInverse._34 },
			0.5f, a_settings.DiffuseIntensity, a_settings.SpecularIntensity, (a_settings.Diffuse ? 1u : 0u) | (a_settings.Specular ? 2u : 0u),
			0u, a_settings.EarlyShadowCull ? 1u : 0u, a_settings.Debug ? 1u : 0u, a_settings.CompareCulling ? 1u : 0u, a_settings.NormalInfluence, camera.CameraProjInverse, camera.CameraViewInverse,
			a_lab.AbsoluteTolerance, a_lab.RelativeTolerance, a_lab.MaskThreshold, a_lab.MaskDistanceTolerance,
			a_lab.DepthAgreement ? (a_lab.DiagnosticDepth ? 1u : 0u) : 2u, a_lab.MaterialPolicy,
			slot->scene.shadow ? 1u : 0u, !a_lab.RequireMaskAuthority && a_lab.AssumeMaskAuthoritative ? 1u : 0u,
			a_lab.ReferenceVisibility ? 1u : 0u, a_lab.ReferencePrimaryTraversal ? 1u : 0u, slot->scene.frame, a_settings.TraceDensityMode,
			{ a_settings.PointLightRadiusMultiplier, a_settings.PointLightFalloffFraction, a_settings.PointLightFalloffExponent, a_settings.DirectStrength },
			{ a_settings.ShadowStrength, a_settings.EmitterRadiusMultiplier, a_settings.StochasticShadows ? 1.0f : 0.0f, a_settings.EmissionStrength },
			slot->rayMaterials ? 1u : 0u, { a_settings.ForcePositiveValidity ? 1u : 0u,
				slot->rayMaterials && slot->rayMaterials->forcedAdmission ? 1u : 0u },
			(globals::features::nativePBR.nativeParallax.load(std::memory_order_relaxed) ? NativePBR::kRayMaterialOptionParallax : 0u) |
			(globals::features::nativePBR.nativeComplexMaterials.load(std::memory_order_relaxed) ? NativePBR::kRayMaterialOptionComplex : 0u) |
			(snowSettings.followParallaxUV ? NativePBR::kRayMaterialOptionSnowParallaxUV : 0u),
			{ snowSettings.noiseAmount, snowRayCells, snowSettings.sparkleAmount, snowSettings.sparkleDensity },
			{ snowSettings.roughness,
				snowSettings.ultravioletAffected ? snowSettings.ultravioletIntensity : 0.0f,
				snowSettings.colorAffection, snowSettings.specularAmount },
			{ snowSettings.lumpHeight, snowSettings.lumpSize, snowSettings.lumpDensity, snowSettings.flatness },
			{ float(snowSettings.lumpLayers), snowSettings.edgeWaviness, 0.0f, 0.0f } };
		slot->nativePointLightCount = selectedCount;
		slot->nativeMinimumDistance = constants.minimum;
		slot->nativeLightTuning = constants.lightTuning;
		slot->nativeShadowTuning = constants.shadowTuning;
		const D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		DX::ThrowIfFailed(slot->lights->Map(0, &noRead, &mapped));
		std::memcpy(mapped, lights.data(), sizeof(lights));
		slot->lights->Unmap(0, nullptr);
		DX::ThrowIfFailed(slot->constants->Map(0, &noRead, &mapped));
		std::memcpy(mapped, &constants, sizeof(constants));
		slot->constants->Unmap(0, nullptr);
		if (slot->rayMaterials && !snowSettings.coverEligible &&
			(snowTerrainScene.lock() != scene.acceleration || snowTerrainCoverage.size() != scene.acceleration->instances.size())) {
			snowTerrainCoverage = ReadNativeSnowTerrain(*scene.acceleration);
			snowTerrainScene = scene.acceleration;
		}
		DX::ThrowIfFailed(slot->rayBindings->Map(0, &noRead, &mapped));
		if (slot->rayMaterials) {
			std::memcpy(mapped, slot->rayMaterials->instances.data(),
				slot->rayMaterials->instances.size() * sizeof(NativePBR::GpuDrawBinding));
			auto* bindings = static_cast<NativePBR::GpuDrawBinding*>(mapped);
			for (size_t i = 0; i < slot->rayMaterials->instances.size(); ++i) {
				if ((bindings[i].flags & NativePBR::kGpuDrawProjectedSnowCandidate) == 0u)
					continue;
				const float terrain = i < snowTerrainCoverage.size() ?
					float(snowTerrainCoverage[i]) / 255.0f : 0.0f;
				const float coverage = NativePBR::NativeSnowCoverage(terrain, snowSettings.coverEligible);
				bindings[i].flags = NativePBR::WithNativeSnowCoverage(bindings[i].flags, coverage);
			}
		}
		slot->rayBindings->Unmap(0, nullptr);
		const auto step = backend.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		constexpr uint32_t kGeometry = 13;
		constexpr uint32_t kAttributes = kGeometry + DX12Native::ShadowBindings::kMaximumInstances;
		constexpr uint32_t kTextures = kAttributes + DX12Native::ShadowBindings::kMaximumInstances;
		constexpr uint32_t kNativePbrTextures = kTextures + DX12Native::kMaximumTexturePools;
		auto cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv{};
		textureSrv.Format = DXGI_FORMAT_R32_FLOAT;
		textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		textureSrv.Texture2D.MipLevels = 1;
		backend.d3d12Device->CreateShaderResourceView(slot->scene.raster->depth->resource.get(), &textureSrv, cpu);
		cpu.ptr += step;
		textureSrv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		backend.d3d12Device->CreateShaderResourceView(slot->scene.shadow ? slot->scene.shadow->mask->resource.get() : nullptr, &textureSrv, cpu);
		cpu.ptr += step;
		for (size_t i = 0; i < 11; ++i) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			if (i == 4) {
				uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
				uav.Format = DXGI_FORMAT_UNKNOWN;
				uav.Buffer.NumElements = uint32_t(totals.size());
				uav.Buffer.StructureByteStride = sizeof(uint32_t);
				backend.d3d12Device->CreateUnorderedAccessView(slot->counters.get(), nullptr, &uav, cpu);
			} else {
				const size_t textureIndex = i < 4 ? i : i - 1;
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uav.Format = formats[textureIndex];
				backend.d3d12Device->CreateUnorderedAccessView(slot->textures[textureIndex].get(), nullptr, &uav, cpu);
			}
			cpu.ptr += step;
		}
		if (slot->descriptorBindings.lock() != scene.materials) {
			cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += uint64_t(kGeometry) * step;
			for (const auto& bottom : scene.acceleration->bottoms) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_R32_TYPELESS;
				srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Buffer.NumElements = uint32_t(bottom->geometry->buffer->GetDesc().Width / 4);
				srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
				backend.d3d12Device->CreateShaderResourceView(bottom->geometry->buffer.get(), &srv, cpu);
				cpu.ptr += step;
			}
			cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += uint64_t(kAttributes) * step;
			for (const auto& instance : scene.acceleration->instances) {
				const auto attributes = instance.material.attributes;
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_R32_TYPELESS;
				srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Buffer.NumElements = attributes ? uint32_t(attributes->buffer->GetDesc().Width / 4) : 1;
				srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
				backend.d3d12Device->CreateShaderResourceView(attributes ? attributes->buffer.get() : nullptr, &srv, cpu);
				cpu.ptr += step;
			}
			cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += uint64_t(kTextures) * step;
			for (uint32_t i = 0; i < DX12Native::kMaximumTexturePools; ++i) {
				const auto* pool = i < scene.materials->materials->pools.size() ? &scene.materials->materials->pools[i] : nullptr;
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = pool ? pool->format : DXGI_FORMAT_R8G8B8A8_UNORM;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2DArray.MipLevels = pool ? pool->mips : 1;
				srv.Texture2DArray.ArraySize = pool ? pool->slices : 1;
				backend.d3d12Device->CreateShaderResourceView(pool ? pool->resource.get() : nullptr, &srv, cpu);
				cpu.ptr += step;
			}
			slot->descriptorBindings = scene.materials;
		}
		if (slot->rayMaterials && slot->descriptorPbr.lock() != slot->rayMaterials->materials) {
			auto destination = slot->heap->GetCPUDescriptorHandleForHeapStart();
			destination.ptr += uint64_t(kNativePbrTextures) * step;
			backend.d3d12Device->CopyDescriptorsSimple(NativePBR::kGpuTextureDescriptorCapacity,
				destination, slot->rayMaterials->materials->textureDescriptors->GetCPUDescriptorHandleForHeapStart(),
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			slot->descriptorPbr = slot->rayMaterials->materials;
		}
		DX::ThrowIfFailed(slot->allocator->Reset());
		if (slot->commands)
			DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
		else
			DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot->allocator.get(), pipeline.get(), IID_PPV_ARGS(slot->commands.put())));
		auto* commands = slot->commands.get();
		commands->CopyBufferRegion(slot->counters.get(), 0, slot->counterReset.get(), 0, totals.size() * sizeof(uint32_t));
		std::vector<D3D12_RESOURCE_BARRIER> begin;
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->scene.raster->depth->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		if (slot->scene.shadow)
			begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->scene.shadow->mask->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		for (const auto& texture : slot->textures)
			if (texture)
				begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(texture.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		commands->ResourceBarrier(uint32_t(begin.size()), begin.data());
		ID3D12DescriptorHeap* heaps[]{ slot->heap.get() };
		commands->SetDescriptorHeaps(1, heaps);
		commands->SetComputeRootSignature(root.get());
		commands->SetComputeRootShaderResourceView(0, scene.acceleration->result->GetGPUVirtualAddress());
		const bool useCharacter = slot->characterCasters &&
		                          slot->characterCasters->Ready() && slot->characterCasters->acceleration;
		commands->SetComputeRootShaderResourceView(12,
			useCharacter ? slot->characterCasters->acceleration->GetGPUVirtualAddress() :
						   scene.acceleration->result->GetGPUVirtualAddress());
		const uint32_t characterModes[2]{
			useCharacter && a_settings.CharacterCasterOcclusion ? 1u : 0u,
			useCharacter && slot->characterProbeEnabled ? 1u : 0u
		};
		commands->SetComputeRoot32BitConstants(13, 2, characterModes, 0);
		commands->SetComputeRootShaderResourceView(2, scene.materials->offsets->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(3, slot->lights->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(5, scene.materials->materials->surfaces->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(6, scene.materials->materials->materials->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(14, slot->rayBindings->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(15, slot->rayMaterials ?
			slot->rayMaterials->materials->records->GetGPUVirtualAddress() :
			scene.materials->materials->materials->GetGPUVirtualAddress());
		commands->SetComputeRootConstantBufferView(11, slot->constants->GetGPUVirtualAddress());
		auto gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
		commands->SetComputeRootDescriptorTable(1, gpu);
		gpu.ptr += step;
		commands->SetComputeRootDescriptorTable(9, gpu);
		gpu.ptr += step;
		commands->SetComputeRootDescriptorTable(10, gpu);
		gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(kGeometry) * step;
		commands->SetComputeRootDescriptorTable(4, gpu);
		gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(kAttributes) * step;
		commands->SetComputeRootDescriptorTable(7, gpu);
		gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(kTextures) * step;
		commands->SetComputeRootDescriptorTable(8, gpu);
		gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(kNativePbrTextures) * step;
		commands->SetComputeRootDescriptorTable(16, gpu);
		commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		std::vector<D3D12_RESOURCE_BARRIER> end;
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->scene.raster->depth->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		if (slot->scene.shadow)
			end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->scene.shadow->mask->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		for (const auto& texture : slot->textures)
			if (texture)
				end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(texture.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		commands->ResourceBarrier(uint32_t(end.size()), end.data());
		commands->CopyBufferRegion(slot->counterReadback.get(), 0, slot->counters.get(), 0, totals.size() * sizeof(uint32_t));
		const auto reset = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		commands->ResourceBarrier(1, &reset);
		DX::ThrowIfFailed(commands->Close());
		DX::ThrowIfFailed(backend.commandQueue->Wait(slot->scene.raster->readyFence.get(), slot->scene.raster->readyValue));
		if (slot->scene.shadow)
			DX::ThrowIfFailed(backend.commandQueue->Wait(slot->scene.shadow->readyFence.get(), slot->scene.shadow->readyValue));
		ID3D12CommandList* lists[]{ commands };
		slot->readyValue = UINT64_MAX;
		slot->countersRead = false;
		backend.commandQueue->ExecuteCommandLists(1, lists);
		const auto ready = ++nextFence;
		DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), ready));
		slot->readyValue = ready;
		globals::features::dx12NativeScene.RetainRasterFrame(slot->scene.raster, completion.get(), ready);
		if (slot->scene.shadow)
			globals::features::dx12NativeScene.RetainShadowMask(slot->scene.shadow, completion.get(), ready);
		current = slot;
		active = true;
		++submissions;
		if (a_settings.CompareCulling)
			++comparisonFrames;
		status.clear();
	} catch (const std::exception& error) {
		status = error.what();
	}
}
