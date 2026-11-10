#include "DX12NativeScene.h"
#include "RE/B/BSTriShape.h"
#include "Globals.h"
#include "TruePBR.h"
#include "Features/LightLimitFix.h"
#include <BS_thread_pool.hpp>
#include "Features/NativeComposite/NativeComposite.h"
#include "GpuPass.h"
#include "State.h"
#include "ShaderCache.h"
#include "Utils/DevBenchUx.h"
#include "Utils/D3D.h"
#include <cmath>
#include <string_view>
#include <utility>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12NativeScene::Settings, Enabled, ShadowMask, NativePBR, NativeLLF, NativeSCM, BudgetMB, BudgetPolicyVersion, FrameBudgetMB, MaterialBatchMB, UseLivePBR, ImportLoosePBR, ImportArchivePBR, RuntimeOnlyMaterials, CaptureRuntimeGeometry, CacheRuntimeGeometry, ShadowAllLights, ShadowStochastic, ShadowDistanceFade, ShadowLightIndex, ShadowNormalInfluence, ShadowEmitterRadius, ShadowReachMultiplier, ShadowFalloffFraction, ShadowFalloffExponent, ShadowTraceDensityMode)


namespace
{
    // Count each shared BLAS once; a combined scene must not double-count
    // memory merely because multiple TLAS instances refer to one bottom level.
    uint64_t NativeAccelerationResidentBytes(const std::shared_ptr<const DX12Native::TopLevel>& scene)
    {
        if (!scene) return 0;
        uint64_t total = scene->bytes;
        std::unordered_set<const DX12Native::BottomLevel*> seen;
        for (const auto& bottom : scene->bottoms)
            if (bottom && seen.insert(bottom.get()).second) total += bottom->bytes;
        return total;
    }

    // Stable, case-insensitive identity for one prepared interior (plugin + local FormID).
    uint64_t PreparedCellReloadToken(std::string_view plugin, uint32_t local)
    {
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : plugin) {
            const unsigned char lower = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
            hash = (hash ^ lower) * 1099511628211ull;
        }
        for (unsigned shift : { 0u, 8u, 16u, 24u })
            hash = (hash ^ ((local >> shift) & 0xffu)) * 1099511628211ull;
        return hash ? hash : 1;
    }

    // Displays only the most recently published JSON snapshot; no GPU queries or
    // file/archive operations are performed by the ImGui thread.
    void DrawNativePBRDiagnostics(const json& a_snapshot)
    {
        const auto it = a_snapshot.find("materialBudget");
        if (it == a_snapshot.end() || !it->is_object()) {
            ImGui::TextDisabled("Native material diagnostics not yet published.");
            return;
        }
        const auto& budget = *it;
        const auto count = [&](const char* a_key) -> uint64_t {
            return budget.value(a_key, uint64_t{ 0 });
        };
        const auto mib = [&](const char* a_key) {
            return double(count(a_key)) / (1024.0 * 1024.0);
        };
        const bool ready = a_snapshot.value("materialBindingsReady", false);
        const bool pending = a_snapshot.value("materialUploadPending", false);
        ImGui::Text("Material bindings: %s", ready ? "Ready" : pending ? "Upload pending" : "Not ready");
        ImGui::TextDisabled("Counters describe the last published material binding set, not individual draw calls.");
        if (!ready) {
            ImGui::TextDisabled("No current binding set; import counters may be zero until the worker completes.");
        }
        const auto failure = a_snapshot.value("materialFailure", std::string{});
        if (!failure.empty()) {
            ImGui::TextWrapped("Material worker failure: %s", failure.c_str());
        }
        const auto textureFailure = budget.value("firstTextureSourceFailure", std::string{});
        if (!textureFailure.empty()) {
            ImGui::TextWrapped("First texture source failure: %s", textureFailure.c_str());
        }

        ImGui::SeparatorText("Live material matching");
        ImGui::Text("Matched: %llu  Applied: %llu  Emission applied: %llu",
            static_cast<unsigned long long>(count("livePbrMatched")),
            static_cast<unsigned long long>(count("livePbrApplied")),
            static_cast<unsigned long long>(count("livePbrEmissiveApplied")));
        ImGui::Text("Missing: %llu  Ambiguous: %llu  Incompatible: %llu",
            static_cast<unsigned long long>(count("livePbrMissing")),
            static_cast<unsigned long long>(count("livePbrAmbiguous")),
            static_cast<unsigned long long>(count("livePbrIncompatible")));
        ImGui::Text("Texture sources - missing: %llu  ambiguous: %llu  incompatible: %llu",
            static_cast<unsigned long long>(count("livePbrSourcesMissing")),
            static_cast<unsigned long long>(count("livePbrSourcesAmbiguous")),
            static_cast<unsigned long long>(count("livePbrSourcesIncompatible")));
        ImGui::Text("Runtime fallbacks: %llu  unsupported: %llu  unsafe alpha: %llu",
            static_cast<unsigned long long>(count("livePbrOfflineFallback")),
            static_cast<unsigned long long>(count("livePbrUnsupported")),
            static_cast<unsigned long long>(count("livePbrUnsafeOpacity")));
        ImGui::Text("Missing basis: %llu  missing bindings: %llu",
            static_cast<unsigned long long>(count("livePbrMissingBasis")),
            static_cast<unsigned long long>(count("livePbrMissingBindings")));
        if (ImGui::TreeNode("Unsupported recipe reasons (overlapping)")) {
            const auto issues = budget.value("livePbrRecipeIssues", std::vector<uint32_t>{});
            const auto flags = budget.value("livePbrFeatureFlags", std::vector<uint32_t>{});
            const auto issue = [&](size_t i) { return i < issues.size() ? issues[i] : 0u; };
            const auto flag = [&](size_t i) { return i < flags.size() ? flags[i] : 0u; };
            ImGui::Text("Recipes with limitations: %llu", static_cast<unsigned long long>(count("livePbrRecipeIssueCount")));
            ImGui::Text("Required maps missing: %u  invalid paths: %u  invalid snapshot: %u", issue(0), issue(12), issue(5));
            ImGui::Text("PBR feature flags: %u  glint: %u  projection: %u", issue(2), issue(3), issue(8));
            ImGui::Text("Displacement: %u  feature textures: %u  landscape: %u", issue(6), issue(7), issue(1));
            ImGui::Text("Subsurface: %u  two-layer: %u  colored coat: %u  parallax: %u",
                flag(0), flag(1), flag(2), flag(3));
            ImGui::Text("Coat normal: %u  fuzz: %u  hair: %u", flag(4), flag(5), flag(6));
            ImGui::TextDisabled("Counts overlap; a recipe may trigger multiple reasons.");
            ImGui::TreePop();
        }
        ImGui::Text("Ready native records: %llu  Rejected: %llu",
            static_cast<unsigned long long>(count("readyMaterials")),
            static_cast<unsigned long long>(count("rejectedMaterials")));

        ImGui::SeparatorText("Texture residency");
        ImGui::Text("Runtime-only source mode: %s",
            budget.value("runtimeOnlyMaterialSources", false) ? "Enabled" : "Disabled");
        ImGui::Text("Loose DDS - planned: %llu  imported: %llu  rejected: %llu  deferred: %llu",
            static_cast<unsigned long long>(count("looseDdsPlanned")),
            static_cast<unsigned long long>(count("looseDdsImported")),
            static_cast<unsigned long long>(count("looseDdsRejected")),
            static_cast<unsigned long long>(count("looseDdsDeferred")));
        ImGui::Text("Eligible recipes: %llu  feature-limited: %llu  skipped: %llu",
            static_cast<unsigned long long>(count("looseDdsReadyRecipes")),
            static_cast<unsigned long long>(count("looseDdsFeatureLimitedRecipes")),
            static_cast<unsigned long long>(count("looseDdsSkippedRecipes")));
        ImGui::Text("Streaming batches: %llu  tried: %llu  resident: %llu  failed: %llu",
            static_cast<unsigned long long>(count("streamBatches")),
            static_cast<unsigned long long>(count("streamAttempted")),
            static_cast<unsigned long long>(count("streamSuccessful")),
            static_cast<unsigned long long>(count("streamFailed")));
        ImGui::Text("Skipped prior attempts: %llu  next batch pending: %s",
            static_cast<unsigned long long>(count("looseDdsPreviouslyAttempted")),
            count("streamNextBatchPending") ? "Yes" : "No");
        ImGui::Text("Already prepared paths: %llu  invalid requests: %llu",
            static_cast<unsigned long long>(count("looseDdsPrepared")),
            static_cast<unsigned long long>(count("looseDdsInvalid")));
        ImGui::Text("Archive DDS - planned: %llu  imported: %llu  rejected: %llu",
            static_cast<unsigned long long>(count("archiveDdsPlanned")),
            static_cast<unsigned long long>(count("archiveDdsImported")),
            static_cast<unsigned long long>(count("archiveDdsRejected")));
        ImGui::Text("Source bytes - loose: %.2f MiB  archive: %.2f MiB",
            mib("looseDdsBytes"), mib("archiveDdsBytes"));
        ImGui::Text("Resident textures: %llu  rejected sources: %llu",
            static_cast<unsigned long long>(count("textures")),
            static_cast<unsigned long long>(count("rejectedTextureSources")));
        ImGui::Text("Uploaded pages: %llu  reused pages: %llu  reused slices: %llu",
            static_cast<unsigned long long>(count("uploadedPages")),
            static_cast<unsigned long long>(count("reusedPages")),
            static_cast<unsigned long long>(count("reusedTextures")));
        ImGui::Text("Disk cache - hits: %llu  writes: %llu",
            static_cast<unsigned long long>(count("diskCacheHits")),
            static_cast<unsigned long long>(count("diskCacheWrites")));

        ImGui::SeparatorText("DX11 texture transfer eligibility");
        ImGui::Text("Unobserved: %llu  unsupported: %llu",
            static_cast<unsigned long long>(count("liveGpuUnobserved")),
            static_cast<unsigned long long>(count("liveGpuUnsupported")));
        ImGui::Text("Copy candidates: %llu  shared-handle candidates: %llu",
            static_cast<unsigned long long>(count("liveGpuCopyCandidates")),
            static_cast<unsigned long long>(count("liveGpuSharedCandidates")));
        ImGui::TextDisabled("These counts are descriptor classifications; no DX11 GPU texture transfer occurs.");

        ImGui::SeparatorText("DX12 memory and uploads");
        ImGui::Text("DXGI budget available: %s", budget.value("dxgiAvailable", false) ? "Yes" : "No");
        ImGui::Text("Local budget: %.1f MiB  usage: %.1f MiB  effective material limit: %.1f MiB",
            mib("localBudgetBytes"), mib("localUsageBytes"), mib("effectiveBytes"));
        ImGui::Text("Textures: %.2f MiB  tables: %.2f MiB  peak staging: %.2f MiB",
            mib("textureBytes"), mib("tableBytes"), mib("peakStagingBytes"));
        const auto retained = budget.find("retainedPageBytes");
        if (retained != budget.end() && retained->is_number_unsigned()) {
            ImGui::Text("Retained pages: %.2f MiB", double(retained->get<uint64_t>()) / (1024.0 * 1024.0));
        } else {
            ImGui::TextDisabled("Retained pages: unavailable");
        }
        ImGui::Text("Uploaded: %.2f MiB  async batch pages: %llu  batch staging: %.2f MiB",
            mib("uploadedBytes"),
            static_cast<unsigned long long>(count("asyncBatchPages")),
            mib("asyncBatchStagingBytes"));
        ImGui::Text("Material upload wait: %.3f ms", double(count("uploadWaitMicroseconds")) / 1000.0);
    }
}

bool DX12NativeScene::OwnsNativeFeatures() const
{
	const auto& upscaling = globals::features::upscaling;
	return loaded && settings.Enabled && !transitionOpen && backend.has_value() &&
		!globals::game::isVR && upscaling.settings.nativeUpscaleEnable;
}

bool DX12NativeScene::IsPreparedStaticCaster(RE::BSGeometry& geometry) const
{
	if (!OwnsNativeFeatures() || !settings.NativeSCM || geometry.GetGeometryRuntimeData().skinInstance)
		return false;
	// Coverage describes actual acceleration instances, never the offline
	// manifest alone. Unmatched static geometry remains in SCM's raster path.
	const auto coverage = runtimePreparedCoverage.load(std::memory_order_acquire);
	if (!coverage || coverage->sceneRevision != revision ||
		runtimeCoverageSource != currentSceneStableAcceleration) return false;
	const auto* reference = geometry.GetUserData();
	if (!reference || reference->IsDynamicForm() || geometry.name.empty()) return false;
	const auto* file = reference->GetFile(0);
	if (!file) return false;
	return coverage->Contains(
		std::format("{:06X}:{}", reference->GetLocalFormID(), file->GetFilename()), geometry.name.c_str());
}

std::shared_ptr<const ShadowCasterManager::NativeCasterFrame> DX12NativeScene::GetNativeSCMFrame(uint32_t a_frame) const
{
	return OwnsNativeFeatures() && settings.NativeSCM ?
		ShadowCasterManager::GetNativeCasterFrame(a_frame) : nullptr;
}

std::optional<DX12NativeScene::NativeLightBatch> DX12NativeScene::GetNativeLLFLights(uint32_t a_frame) const
{
	if (!OwnsNativeFeatures()) return std::nullopt; // Legacy/prepared lighting owns non-native frames.
	NativeLightBatch batch{};
	if (!settings.NativeLLF) return batch; // Explicit opt-out; never changes renderer ownership.
	const auto published = globals::features::lightLimitFix.GetNativeLightFrame(a_frame);
	if (!published) return batch; // Current frame has not been published yet.
	batch.observed = static_cast<uint32_t>(published->lights.size());
	for (const auto& light : published->lights) {
		if (batch.count == batch.lights.size()) break;
		batch.lights[batch.count++] = { light.position, light.radiance, light.radius, light.sourceRadius, true };
	}
	return batch;
}

void DX12NativeScene::DrawSettings()
{
	std::scoped_lock lock(settingsMutex);
	if (ImGui::Button("Analyze live PBR candidates")) coverageRequested = true;
    ImGui::TextDisabled("Native DX12 ownership: %s", OwnsNativeFeatures() ? "active" : "inactive");
    if (ImGui::Checkbox("NativePBR (runtime material bridge)", &settings.NativePBR)) resetRequested = true;
    ImGui::Checkbox("NativeLLF (live light bridge)", &settings.NativeLLF);
    ImGui::Checkbox("NativeSCM (dynamic caster bridge)", &settings.NativeSCM);
    ImGui::TextDisabled("No per-effect validity fallback to legacy execution.");
    ImGui::BeginDisabled(!settings.NativePBR);
    if (ImGui::Checkbox("Experimental: import missing loose PBR DDS", &settings.ImportLoosePBR)) resetRequested = true;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!settings.NativePBR || !settings.ImportLoosePBR);
    if (ImGui::Checkbox("Experimental: use archive-backed DDS fallback", &settings.ImportArchivePBR))
        resetRequested = true;
    ImGui::EndDisabled();
    ImGui::TextDisabled("Worker-only; maximum 8 sources / 32 MiB combined per load.");
    ImGui::TextDisabled("Archive fallback runs only when the loose DDS is absent.");
    ImGui::BeginDisabled(!settings.NativePBR || !settings.ImportLoosePBR);
    ImGui::Checkbox("Experimental: runtime-only material sources (no atlas manifest)",
        &settings.RuntimeOnlyMaterials);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Prepared geometry is still required; unsupported materials can lack native shading.");
    if (ImGui::Checkbox("Experimental: capture runtime static geometry", &settings.CaptureRuntimeGeometry))
        runtimeGeometryCaptureActive.store(settings.CaptureRuntimeGeometry, std::memory_order_release);
    ImGui::TextDisabled("Copies CPU-backed static mesh bytes only; GPU-only/skinned/dynamic meshes are excluded.");
    ImGui::BeginDisabled(!settings.CaptureRuntimeGeometry);
    ImGui::Checkbox("Experimental: persist and menu-prewarm runtime geometry",
        &settings.CacheRuntimeGeometry);
    ImGui::EndDisabled();
    ImGui::TextDisabled("CPU-only prewarm; reuse requires identical live geometry after scene entry.");
    ImGui::Text("Geometry cache: saved %u  failed %u  prewarmed %u / %u (%.2f MiB)%s",
        runtimeGeometryCache.Stored(), runtimeGeometryCache.Rejected(),
        runtimeGeometryCache.MenuAccepted(), runtimeGeometryCache.MenuChecked(),
        double(runtimeGeometryCache.MenuBytes()) / (1024.0 * 1024.0),
        runtimeGeometryCache.Busy() ? "  [working]" : "");
    if (!runtimeGeometryCache.Failure().empty())
        ImGui::TextWrapped("Geometry cache: %s", runtimeGeometryCache.Failure().c_str());
    ImGui::Text("Cache-assisted geometry encodes: %u", runtimeGeometryPrepare.CacheHits());
    ImGui::Text("Runtime mesh captures: %zu  bytes: %.2f MiB",
        runtimeGeometry.Count(), double(runtimeGeometry.Bytes()) / (1024.0 * 1024.0));
    ImGui::Text("Prepared native meshes: %zu  GPU bytes: %.2f MiB",
        runtimeGeometryPrepare.GetCell() && runtimeGeometryPrepare.GetCell()->snapshot ?
            runtimeGeometryPrepare.GetCell()->snapshot->instances.size() : 0,
        runtimeGeometryPrepare.GetCell() ? double(runtimeGeometryPrepare.GetCell()->bytes) / (1024.0 * 1024.0) : 0.0);
    ImGui::Text("Runtime geometry batches: %u  attempts: %zu  failures: %u",
        runtimeGeometryPrepare.Batches(), runtimeGeometryPrepare.Attempted(),
        runtimeGeometryPrepare.Failures());
    ImGui::Text("Current-scene TLAS: %s",
        currentSceneStableAcceleration && currentSceneStableAcceleration->scope.policy == 2 ?
            "Prepared + runtime geometry" : "Prepared geometry only");
    ImGui::TextDisabled("Runtime shapes enter the production scene after completion and material binding.");
    if (ImGui::CollapsingHeader("Native PBR Diagnostics"))
        DrawNativePBRDiagnostics(GetDiagnostics());
	{
		std::scoped_lock coverageLock(coverageMutex);
		if (coverageFailed) ImGui::TextDisabled("Candidate analysis unavailable or failed.");
		if (coverageTask) ImGui::TextDisabled("Analyzing live PBR candidates...");
		if (coverageResult) {
			const auto scene = GetPublishedScene();
			ImGui::TextDisabled("Unverified snapshot candidates%s: %zu unique, %zu ambiguous, %zu unobserved, %zu without source identity",
				!scene || scene->revision != coverageRevision ? " (previous scene)" : "", coverageResult->uniqueCandidates,
				coverageResult->ambiguous, coverageResult->noObservation, coverageResult->missingIdentity);
			if (coverageResult->recipeGenerationAvailable)
				ImGui::TextDisabled("Candidate recipes at revision %llu: %zu available, %zu pending or unavailable",
					static_cast<unsigned long long>(coverageResult->recipeRevision), coverageResult->candidateRecipes, coverageResult->candidateRecipesMissing);
		}
	}
	if (ImGui::Checkbox("Load prepared scene", &settings.Enabled)) resetRequested = true;
	if (ImGui::Checkbox("Produce RT shadow mask", &settings.ShadowMask)) shadowResetRequested = true;
	if (settings.ShadowMask && !shadowStatus.empty()) ImGui::TextUnformatted(shadowStatus.c_str());
	if (settings.ShadowMask) ImGui::TextDisabled("Opaque-frame shadow recoveries: %llu / %llu",
		static_cast<unsigned long long>(shadowOpaqueRetryRecovered),
		static_cast<unsigned long long>(shadowOpaqueRetryAttempts));
	if (ImGui::CollapsingHeader("Early DX12 shadow-mask controls")) {
		bool changed = false;
		ImGui::TextDisabled("This panel owns raw shadow visibility; its final-image contribution is independent in Native Composite.");
		int traceDensity = int(settings.ShadowTraceDensityMode);
		if (ImGui::Combo("Independent shadow trace density", &traceDensity, "Full\0Two-thirds\0Half\0One-third\0Quarter\0One-sixth\0One-ninth\0Four-ninths\0")) {
			settings.ShadowTraceDensityMode = uint32_t(traceDensity);
			changed = true;
		}
		ImGui::TextDisabled("All modes trace at render size; skipped pixels have validity -1.");
		changed |= ImGui::Checkbox("Shadow all prepared point lights", &settings.ShadowAllLights);
		if (!settings.ShadowAllLights) {
			const auto scene = GetPublishedScene();
			int index = int(settings.ShadowLightIndex);
			const int maxIndex = scene && !scene->cell->nativeLights.empty() ? int(scene->cell->nativeLights.size()-1) : 0;
			if (ImGui::SliderInt("Shadow point light index", &index, 0, maxIndex)) { settings.ShadowLightIndex = uint32_t(index); changed = true; }
		}
		changed |= ImGui::Checkbox("Stochastic shadow emitter sampling", &settings.ShadowStochastic);
		ImGui::BeginDisabled(!settings.ShadowStochastic);
		changed |= ImGui::SliderFloat("Shadow emitter radius multiplier", &settings.ShadowEmitterRadius, 0.0f, 8.0f);
		ImGui::EndDisabled();
		changed |= ImGui::SliderFloat("Shadow light reach multiplier", &settings.ShadowReachMultiplier, 0.1f, 64.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		changed |= ImGui::SliderFloat("Shadow fade range fraction", &settings.ShadowFalloffFraction, 0.02f, 1.0f);
		changed |= ImGui::SliderFloat("Shadow falloff curve", &settings.ShadowFalloffExponent, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		changed |= ImGui::Checkbox("Fade shadows near light reach", &settings.ShadowDistanceFade);
		int normal = int(settings.ShadowNormalInfluence);
		if (ImGui::Combo("Shadow normal influence", &normal, "Legacy flat\0Geometry normals\0Full material normals\0"))
			{ settings.ShadowNormalInfluence = uint32_t(normal); changed = true; }
		if (ImGui::Button("Reset shadow reach / falloff")) {
			const Settings defaults{};
			settings.ShadowReachMultiplier = defaults.ShadowReachMultiplier;
			settings.ShadowFalloffFraction = defaults.ShadowFalloffFraction;
			settings.ShadowFalloffExponent = defaults.ShadowFalloffExponent;
			settings.ShadowDistanceFade = defaults.ShadowDistanceFade;
			changed = true;
		}
		if (changed) ++shadowControlsRevision;
	}
    ImGui::SeparatorText("Prepared scene VRAM budget");
    const uint32_t pendingSceneBudget = requestedBudget.load();
    uint32_t selectedSceneBudget = pendingSceneBudget != UINT32_MAX ?
        pendingSceneBudget : settings.BudgetMB;
    bool automaticSceneBudget = selectedSceneBudget == 0;
    if (ImGui::Checkbox("Automatic prepared scene budget (DXGI)", &automaticSceneBudget)) {
        requestedBudget.store(automaticSceneBudget ? 0u : 2048u);
        resetRequested.store(true, std::memory_order_release);
        selectedSceneBudget = automaticSceneBudget ? 0u : 2048u;
    }
    if (!automaticSceneBudget) {
        int cap = int(selectedSceneBudget);
        if (ImGui::SliderInt("Manual scene budget (MiB)", &cap, 64, 4096)) {
            requestedBudget.store(uint32_t(cap));
            resetRequested.store(true, std::memory_order_release);
        }
    }
    const auto budgetInfo = GetDiagnostics().value("preparedSceneBudget", json::object());
    ImGui::Text("DXGI local usage: %.1f / %.1f MiB | effective scene allowance: %.1f MiB",
        double(budgetInfo.value("localUsageBytes", uint64_t{0})) / (1024.0 * 1024.0),
        double(budgetInfo.value("localBudgetBytes", uint64_t{0})) / (1024.0 * 1024.0),
        double(budgetInfo.value("effectiveBytes", uint64_t{0})) / (1024.0 * 1024.0));
    ImGui::TextDisabled("Budget applies to prepared geometry, acceleration and material stages. Zero selects automatic DXGI headroom.");
    ImGui::SeparatorText("Prepared DX12 CELL loading");
    if (ImGui::Button("Reload prepared CELL from cache"))
        resetRequested.store(true, std::memory_order_release);
    ImGui::TextDisabled("Read the published 3-file CELL package without running preparation.");
    {
        // UI only reads a cached JSON snapshot; disk IO and DX12 work stay off the menu thread.
        const auto snapshot = GetDiagnostics();
        const auto preparedStatus = snapshot.value("prepared", json::object());
        const auto as = snapshot.value("acceleration", json::object());
        const auto value = [](const json& node, const char* key) -> uint64_t {
            return node.is_object() ? node.value(key, uint64_t{0}) : 0;
        };
        ImGui::Text("CELL loading: %s",
            preparedStatus.value("loading", false) ? "In progress" :
            preparedStatus.value("loaded", false) ? "Geometry loaded" :
            preparedStatus.value("requested", false) ? "Attempt finished" : "Not requested");
        ImGui::Text("Geometry upload progress: %llu meshes / %.2f MiB",
        static_cast<unsigned long long>(value(preparedStatus, "uploadProgressMeshes")),
        double(value(preparedStatus, "uploadProgressBytes")) / (1024.0 * 1024.0));
    ImGui::Text("Geometry: %llu meshes / %llu instances / %.2f MiB",
            static_cast<unsigned long long>(value(preparedStatus, "meshes")),
            static_cast<unsigned long long>(value(preparedStatus, "instances")),
            double(value(preparedStatus, "geometryBytes")) / (1024.0 * 1024.0));
        ImGui::Text("Shading: %llu instances / %llu tangent frames",
            static_cast<unsigned long long>(value(preparedStatus, "shadedInstances")),
            static_cast<unsigned long long>(value(preparedStatus, "tangentInstances")));
        ImGui::Text("BLAS: %llu/%llu | TLAS: %llu/%llu",
            static_cast<unsigned long long>(value(as, "bottomCompletions")),
            static_cast<unsigned long long>(value(as, "bottomSubmissions")),
            static_cast<unsigned long long>(value(as, "topCompletions")),
            static_cast<unsigned long long>(value(as, "topSubmissions")));
        ImGui::Text("Published DX12 scene: %s", snapshot.value("published", false) ? "Yes" : "No");
        const auto loadFailure = preparedStatus.value("failure", std::string{});
        if (!loadFailure.empty()) ImGui::TextWrapped("CELL load failure: %s", loadFailure.c_str());
        const auto asFailure = as.value("failure", std::string{});
        if (!asFailure.empty()) ImGui::TextWrapped("Acceleration failure: %s", asFailure.c_str());
        const auto workerFailure = as.value("lastBatchFailure", std::string{});
        if (!workerFailure.empty()) ImGui::TextWrapped("AS worker failure: %s", workerFailure.c_str());
        ImGui::TextDisabled("Geometry and acceleration are reported independently of material readiness.");
    }
	ImGui::TextUnformatted(failure.empty() ? "Prepared static scenes" : failure.c_str());
	const uint32_t requestedFrame = requestedFrameBudget.load();
	uint32_t frameLimit = requestedFrame != UINT32_MAX ? requestedFrame : settings.FrameBudgetMB;
	bool automatic = frameLimit == 0;
	if (ImGui::Checkbox("Automatic native frame VRAM budget (DXGI)", &automatic)) {
		frameLimit = automatic ? 0u : 512u;
		requestedFrameBudget = frameLimit;
	}
	if (!automatic) {
		int manual = int(frameLimit);
		if (ImGui::SliderInt("Native frame-pool limit (MiB)", &manual, 64, 4096))
			requestedFrameBudget = uint32_t(manual);
	}
	ImGui::TextDisabled("Pool allocations respect DXGI headroom; independent of the prepared-scene budget.");
	const auto requestedBatch = requestedMaterialBatch.load();
	int batchMB = int(requestedBatch ? requestedBatch : settings.MaterialBatchMB);
	if (ImGui::SliderInt("Prepared material batch limit (MiB)", &batchMB, 1, 64)) requestedMaterialBatch = uint32_t(batchMB);
	ImGui::TextDisabled("Applies to the next prepared material load; larger sets use the existing worker path.");
}

void DX12NativeScene::LoadSettings(json& a_json)
{
	const Settings selected = a_json;
    // Configs from before automatic budgeting stored the old 512 MiB
    // default. Migrate that default once, preserving explicit new overrides.
    const uint32_t restoredSceneBudget =
        !a_json.contains("BudgetPolicyVersion") && selected.BudgetMB == 512 ?
        0u : selected.BudgetMB;
	// A compositor requesting the independent layer takes precedence over the
	// scene configuration's possibly older disabled producer state.
	if (requestedEnabled.load() != 1) requestedEnabled = selected.Enabled ? 1 : 0;
	if (requestedShadow.load() != 1) requestedShadow = selected.ShadowMask ? 1 : 0;
	requestedBudget = restoredSceneBudget ? std::clamp(restoredSceneBudget, 64u, 4096u) : 0u;
	requestedFrameBudget = selected.FrameBudgetMB ? std::clamp(selected.FrameBudgetMB, 64u, 4096u) : 0u;
	requestedMaterialBatch = DX12Native::ClampMaterialBatchMB(selected.MaterialBatchMB);
	std::scoped_lock lock(settingsMutex);
	const auto clamp = [](float value, float fallback, float low, float high) { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; };
	++shadowControlsRevision;
	settings.UseLivePBR = selected.UseLivePBR;
	settings.NativePBR = selected.NativePBR;
	settings.NativeLLF = selected.NativeLLF;
	settings.NativeSCM = selected.NativeSCM;
        settings.ImportLoosePBR = selected.ImportLoosePBR;
        settings.ImportArchivePBR = selected.ImportArchivePBR;
        settings.RuntimeOnlyMaterials = selected.RuntimeOnlyMaterials;
	settings.ShadowAllLights = selected.ShadowAllLights;
	settings.ShadowStochastic = selected.ShadowStochastic;
	settings.ShadowDistanceFade = selected.ShadowDistanceFade;
	settings.ShadowLightIndex = selected.ShadowLightIndex;
	settings.ShadowNormalInfluence = std::min(selected.ShadowNormalInfluence, 2u);
	settings.ShadowTraceDensityMode = std::min(selected.ShadowTraceDensityMode, 7u);
	settings.ShadowEmitterRadius = clamp(selected.ShadowEmitterRadius, Settings{}.ShadowEmitterRadius, 0.0f, 8.0f);
	settings.ShadowReachMultiplier = clamp(selected.ShadowReachMultiplier, Settings{}.ShadowReachMultiplier, 0.1f, 64.0f);
	settings.ShadowFalloffFraction = clamp(selected.ShadowFalloffFraction, Settings{}.ShadowFalloffFraction, 0.02f, 1.0f);
	settings.ShadowFalloffExponent = clamp(selected.ShadowFalloffExponent, Settings{}.ShadowFalloffExponent, 0.25f, 4.0f);
	resetRequested = true;
}
void DX12NativeScene::SaveSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	auto selected = settings;
	if (const auto enabled = requestedEnabled.load(); enabled >= 0) selected.Enabled = enabled != 0;
	if (const auto shadowEnabled = requestedShadow.load(); shadowEnabled >= 0) selected.ShadowMask = shadowEnabled != 0;
	if (const auto budget = requestedBudget.load(); budget != UINT32_MAX) selected.BudgetMB = budget;
	if (const auto frameBudget = requestedFrameBudget.load(); frameBudget != UINT32_MAX) selected.FrameBudgetMB = frameBudget;
	if (const auto batch = requestedMaterialBatch.load(); batch) selected.MaterialBatchMB = batch;
	a_json = selected;
}
void DX12NativeScene::RestoreDefaultSettings()
{
	requestedEnabled = Settings{}.Enabled ? 1 : 0;
	requestedShadow = Settings{}.ShadowMask ? 1 : 0;
	requestedBudget = Settings{}.BudgetMB;
	requestedFrameBudget = Settings{}.FrameBudgetMB;
	requestedMaterialBatch = Settings{}.MaterialBatchMB;
	{ std::scoped_lock lock(settingsMutex); settings = Settings{}; ++shadowControlsRevision; }
	resetRequested = true;
}
void DX12NativeScene::OnRuntimeDisabled() { resetRequested = true; }
void DX12NativeScene::OnSceneTransitionReset(bool a_open)
{
	transitionOpen = a_open;
	if (a_open) {
        globals::features::truePBR.nativeRegistry.ClearInstanceHints();
        runtimeGeometry.Reset();
    }
	ResetScene();
}
void DX12NativeScene::GameLoaded() { resetRequested = true; }


void DX12NativeScene::ObserveRuntimeGeometry(RE::BSGeometry* geometry,
    const std::string& reference, const std::string& shape)
{
    if (!runtimeGeometryCaptureActive.load(std::memory_order_relaxed) || !geometry) return;
    if (const auto covered = runtimePreparedCoverage.load(std::memory_order_acquire);
        covered && covered->Contains(reference, shape)) {
        runtimePreparedSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (runtimeGeometry.Contains(reference, shape)) return;
    // Only static BSTriShape CPU data is supported; never map D3D11 buffers.
    auto* tri = geometry->AsTriShape();
    if (!tri || geometry->AsDynamicTriShape()) return;
    const auto& runtime = geometry->GetGeometryRuntimeData();
    const auto* renderer = runtime.rendererData;
    if (runtime.skinInstance || runtime.alphaProperty || !renderer ||
        !renderer->rawVertexData || !renderer->rawIndexData) return;
    auto desc = runtime.vertexDesc;
    if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) return;
    const uint32_t stride = desc.GetSize();
    const auto& counts = tri->GetTrishapeRuntimeData();
    if (!counts.vertexCount || !counts.triangleCount || stride < 16 || stride > 256) return;
    const uint64_t length = uint64_t(counts.vertexCount) * stride +
        uint64_t(counts.triangleCount) * 3 * sizeof(uint16_t);
    if (length > 256 * 1024) return; // Bound render-thread copy latency.
    const uint32_t frame = globals::game::graphicsState ?
        globals::game::graphicsState->GetFrameCount() : 0;
    {
        std::scoped_lock lock(runtimeGeometryBudgetMutex);
        if (runtimeGeometryBudgetFrame != frame) {
            runtimeGeometryBudgetFrame = frame;
            runtimeGeometryFrameBytes = 0;
        }
        if (length > 512 * 1024 - runtimeGeometryFrameBytes) return;
        runtimeGeometryFrameBytes += uint32_t(length);
    }
    DX12Native::RuntimeGeometryInput input;
    input.referenceKey = reference;
    input.shapeName = shape;
    input.vertexCount = counts.vertexCount;
    input.vertexStride = stride;
    input.triangleCount = counts.triangleCount;
    input.hasNormal = desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL);
    input.hasUv = desc.HasFlag(RE::BSGraphics::Vertex::VF_UV);
    input.hasTangent = desc.HasFlag(RE::BSGraphics::Vertex::VF_TANGENT);
    if (input.hasNormal) input.normalOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
    if (input.hasUv) input.uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
    if (input.hasTangent) input.tangentOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_BINORMAL);
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t col = 0; col < 3; ++col)
            input.transform[4 * row + col] = geometry->world.rotate.entry[row][col] * geometry->world.scale;
    }
    input.transform[3] = geometry->world.translate.x;
    input.transform[7] = geometry->world.translate.y;
    input.transform[11] = geometry->world.translate.z;
    input.bound = { geometry->worldBound.center.x, geometry->worldBound.center.y,
        geometry->worldBound.center.z, geometry->worldBound.radius };
    input.frame = frame;
    input.vertices = { renderer->rawVertexData, size_t(counts.vertexCount) * stride };
    input.indices = { renderer->rawIndexData, size_t(counts.triangleCount) * 3 };
    runtimeGeometry.Submit(DX12Native::CopyRuntimeGeometry(input, 256 * 1024));
}

void DX12NativeScene::NotifyPreparedCellPublished(std::string_view plugin, uint32_t localID)
{
    if (plugin.empty() || !localID) return;
    pendingPreparedCellReload.store(PreparedCellReloadToken(plugin, localID), std::memory_order_release);
}

uint64_t DX12NativeScene::SceneMemoryLimit(uint64_t trackedBytes)
{
    if (!backend) return uint64_t(settings.BudgetMB ? settings.BudgetMB : 512u) *
        DX12Native::VideoMemoryBudget::kMiB;
    const uint64_t manualBytes = uint64_t(settings.BudgetMB) *
        DX12Native::VideoMemoryBudget::kMiB;
    preparedSceneBudgetStatus = preparedSceneVideoMemory.Query(
        backend->d3d12Device.get(), trackedBytes, manualBytes);
    return preparedSceneBudgetStatus.effectiveBytes;
}

void DX12NativeScene::ResetScene()
{
	++revision;
	cellID = 0;
	submitted = false;
	raster.Reset();
	shadow.Reset();
	if (auto old = materialBindings.Get()) retiredMaterials = old;
	materialBindings.Reset();
    materialRequestBudgetScene.reset();
    materialRequestBudgetBytes = 0;
    livePbrSource.reset(); livePbrRevision = 0; livePbrLastCaptureFrame = 0;
    runtimeGeometryPrepare.Reset();
    runtimeGeometry.Reset();
    runtimePreparedCoverage.store({}, std::memory_order_release);
    runtimeCoverageSource.reset();
    runtimePreparedSkipped.store(0, std::memory_order_relaxed);
    runtimePreparedPruned = 0;
    runtimeGeometryCache.ResetScene();
    currentSceneStableAcceleration.reset();
    currentSceneStableBindings.reset();
    currentSceneSubmittedBatch = 0;
    runtimeGeometryBudgetStatus = {};
    preparedSceneBudgetStatus = {};
    runtimeGeometryActiveBytes = runtimeGeometryAllowedBytes = 0;
	prepared.Reset();
	acceleration.Reset();
}

struct DX12NativeScene::CoverageTask
{
	std::atomic<bool> done{ false };
	uint64_t sceneRevision = 0;
	NativePBR::InstanceCoverage coverage;
	bool failed = false;
};

void DX12NativeScene::AdvanceCoverage()
{
	std::scoped_lock lock(coverageMutex);
	if (coverageTask && coverageTask->done.load(std::memory_order_acquire)) {
		coverageFailed = coverageTask->failed;
		if (!coverageFailed) { coverageResult = coverageTask->coverage; coverageRevision = coverageTask->sceneRevision; }
		coverageTask.reset();
	}
	if (coverageTask || !coverageRequested.exchange(false)) return;
	const auto scene = GetPublishedScene();
	if (!scene) { coverageFailed = true; return; }
	try {
		auto task = std::make_shared<CoverageTask>();
		task->sceneRevision = scene->revision;
		auto& pbr = globals::features::truePBR;
		auto recipes = pbr.GetNativeRecipes();
		auto hints = pbr.nativeRegistry.ExportInstanceHints();
		if (recipes && recipes->revision != pbr.nativeRegistry.Revision()) recipes.reset();
		static BS::thread_pool<> pool{ 1 };
		pool.detach_task([task, acceleration = scene->acceleration, hints = std::move(hints), recipes = std::move(recipes)] {
			try {
				std::vector<NativePBR::PreparedInstanceKey> keys;
				keys.reserve(acceleration->instances.size());
				for (const auto& instance : acceleration->instances)
					keys.push_back({ instance.preparedIdentity.referenceKey, instance.preparedIdentity.shapeName });
				task->coverage = NativePBR::AnalyzeInstanceCoverage(keys, hints, recipes.get());
			} catch (...) { task->failed = true; }
			task->done.store(true, std::memory_order_release);
		});
		coverageFailed = false;
		coverageTask = std::move(task);
	} catch (const std::exception& error) { coverageFailed = true; logger::warn("Native PBR candidate analysis: {}", error.what()); }
}

void DX12NativeScene::EarlyPrepass()
{
	CS_GPU_PASS("DX12NativeScene::PreparedSceneProgress");
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	AdvanceCoverage();
	DX12Native::RetireMaterialUploads();
	{
		std::scoped_lock lock(settingsMutex);
		if (const auto enabled = requestedEnabled.exchange(-1); enabled >= 0) settings.Enabled = enabled != 0;
		if (const auto shadowEnabled = requestedShadow.exchange(-1); shadowEnabled >= 0) settings.ShadowMask = shadowEnabled != 0;
		if (const auto budget = requestedBudget.exchange(UINT32_MAX); budget != UINT32_MAX) settings.BudgetMB = budget;
		if (const auto frameBudget = requestedFrameBudget.exchange(UINT32_MAX); frameBudget != UINT32_MAX) settings.FrameBudgetMB = frameBudget;
		if (const auto batch = requestedMaterialBatch.exchange(0); batch) settings.MaterialBatchMB = batch;
	}
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	const auto currentID = cell ? cell->GetFormID() : 0;
    // Publication can complete after an earlier missing-cache attempt.
    // Consume the matching notification on the scene thread, not the companion worker.
    if (const auto notification = pendingPreparedCellReload.exchange(0, std::memory_order_acq_rel);
        notification && cell && cell->IsInteriorCell()) {
        const auto* source = cell->GetFile(0);
        if (source && notification == PreparedCellReloadToken(source->GetFilename(), cell->GetLocalFormID()))
            resetRequested.store(true, std::memory_order_release);
    }
	if (reloadRequested.exchange(false)) {
		ResetScene();
		raster.ReloadShader();
		shadow.ReloadShaders();
	}
	runtimeGeometryCaptureActive.store(settings.Enabled && settings.CaptureRuntimeGeometry,
        std::memory_order_release);
    if (resetRequested.exchange(false)) ResetScene();
    // This CPU-only cache scheduler operates before the native scene/cell
    // early returns, so the menu can prewarm cached meshes without a TLAS.
    // All disk I/O and decoding remain on its background worker.
    if (settings.CacheRuntimeGeometry)
        runtimeGeometryCache.Advance(!cell && !transitionOpen,
            settings.CaptureRuntimeGeometry && cell && !transitionOpen &&
                runtimePreparedCoverage.load(std::memory_order_acquire) ?
                &runtimeGeometry : nullptr);
	if (shadowResetRequested.exchange(false)) shadow.Reset();
	if (!settings.Enabled || transitionOpen) {
		if (cellID) ResetScene();
		return;
	}
	if (!backend) {
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, failure);
		if (!backend) return;
	}
	if (!cell || !cell->IsInteriorCell()) {
		if (cellID) ResetScene();
		return;
	}
	if (cellID != currentID) {
		ResetScene();
		cellID = currentID;
		failure.clear();
	}
    // Runtime geometry shares actual DXGI headroom and the scene's manual
    // ceiling with prepared geometry, completed BLAS/TLAS and materials.
    // Do not allocate a duplicate runtime-only preview acceleration scene.
    if (settings.CaptureRuntimeGeometry && currentSceneStableAcceleration &&
        currentSceneStableAcceleration->scope.scene == revision &&
        runtimePreparedCoverage.load(std::memory_order_acquire) &&
        runtimeGeometry.Count() != 0) {
        const auto currentCell = prepared.GetCell();
        const auto bound = currentSceneStableBindings ? currentSceneStableBindings : materialBindings.Get();
        const uint64_t baselineGeometryBytes = currentCell ? currentCell->bytes : 0;
        const uint64_t baselineAsBytes = NativeAccelerationResidentBytes(currentSceneStableAcceleration);
        const uint64_t baselineMaterialBytes = bound ? bound->bytes : 0;
        runtimeGeometryActiveBytes = baselineGeometryBytes + baselineAsBytes + baselineMaterialBytes;
        const uint64_t totalLimit = SceneMemoryLimit(runtimeGeometryActiveBytes);
        const uint64_t availableManual = totalLimit > runtimeGeometryActiveBytes ?
            totalLimit - runtimeGeometryActiveBytes : 0;
        const auto runtimeCell = runtimeGeometryPrepare.GetCell();
        const uint64_t geometryBytes = runtimeCell ? runtimeCell->bytes : 0;
        runtimeGeometryAllowedBytes = 0;
        if (availableManual) {
            runtimeGeometryBudgetStatus = runtimeGeometryVideoMemory.Query(
                backend->d3d12Device.get(), geometryBytes, availableManual);
            runtimeGeometryAllowedBytes = runtimeGeometryBudgetStatus.effectiveBytes;
        } else runtimeGeometryBudgetStatus = {};
        // Retire completed work even if no further allocations are allowed.
        runtimeGeometryPrepare.Advance(*backend, runtimeGeometry, revision,
            runtimeGeometryAllowedBytes,
            settings.CacheRuntimeGeometry ? runtimeGeometryCache.GetPrewarmed() : nullptr,
            currentCell);
    }
	if (!prepared.Requested()) {
		const auto* source = cell->GetFile(0);
		if (!source) { failure = "Cell source unavailable"; return; }
		const auto local = cell->GetLocalFormID();
		const auto key = std::format("{:06X}:{}", local, source->GetFilename());
		const auto directory = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/DX12-cache/cells") / source->GetFilename() / std::format("{:06X}", local);
        const uint64_t manualBytes = uint64_t(settings.BudgetMB) * DX12Native::VideoMemoryBudget::kMiB;
        SceneMemoryLimit();
		prepared.Request(*backend, directory, key, revision, manualBytes, manualBytes);
	}
	if (!submitted) {
		if (auto scene = prepared.GetCell()) {
            const uint64_t sceneAllowance = SceneMemoryLimit(scene->bytes);
            if (sceneAllowance > scene->bytes) {
                acceleration.Start(*backend, scene->snapshot, scene->geometries,
                    uint64_t(settings.BudgetMB) * DX12Native::VideoMemoryBudget::kMiB);
                submitted = true;
            }
		}
	}
	acceleration.Advance();
    // Keep the last usable scene until the replacement TLAS and its own
    // material bindings are ready; never expose a mismatched resource pair.
    if (auto completed = acceleration.GetScene(); completed &&
        completed->scope.scene == revision && !currentSceneStableAcceleration)
        currentSceneStableAcceleration = completed;
    // Reuse the runtime's existing reference/shape identities, but only skip
    // geometries actually admitted to a completed production TLAS. Offline
    // omissions and preview-only candidates must remain discoverable.
    if (auto current = acceleration.GetScene(); current &&
        current->scope.scene == revision && current != runtimeCoverageSource) {
        auto covered = std::make_shared<DX12Native::PreparedRuntimeCoverage>();
        covered->sceneRevision = revision;
        for (const auto& instance : current->instances) {
            const auto& identity = instance.preparedIdentity;
            if (!identity.referenceKey.empty() && !identity.shapeName.empty())
                covered->completed.insert(identity.referenceKey + '\0' + identity.shapeName);
        }
        runtimeCoverageSource = current;
        runtimePreparedCoverage.store(covered, std::memory_order_release);
        runtimePreparedPruned += runtimeGeometry.RemoveCovered(*covered);
    }
    if (auto scene = acceleration.GetScene(); scene &&
        shadow.RetireScenesExcept(currentSceneStableAcceleration ? currentSceneStableAcceleration : scene) &&
        retiredMaterials.expired()) {
        if (OwnsNativeFeatures() && settings.NativePBR && globals::game::graphicsState) {
            auto& pbr = globals::features::truePBR;
            const uint64_t currentRevision = pbr.nativeRegistry.Revision();
            const uint32_t currentFrame = globals::game::graphicsState->GetFrameCount();
            if ((currentRevision != livePbrRevision || !livePbrSource) &&
                livePbrLastCaptureFrame != currentFrame) {
                livePbrLastCaptureFrame = currentFrame;
                auto recipes = pbr.GetNativeRecipes();
                if (recipes && recipes->batchesComplete) {
                    auto source = std::make_shared<NativePBR::LiveBindingSource>();
                    source->generation = std::move(recipes);
                    source->hints = pbr.nativeRegistry.ExportInstanceHints();
                    if (pbr.nativeRegistry.Revision() == currentRevision) {
                        livePbrSource = std::move(source);
                        livePbrRevision = currentRevision;
                    }
                }
            }
        } else { livePbrSource.reset(); livePbrRevision = 0; }
        // Latch the starting material allowance for each TLAS generation:
        // per-frame DXGI fluctuations must not repeatedly restart uploads.
        if (materialRequestBudgetScene != scene || !materialRequestBudgetBytes) {
            const uint64_t occupied =
                (prepared.GetCell() ? prepared.GetCell()->bytes : 0) +
                NativeAccelerationResidentBytes(scene);
            const uint64_t totalAllowance = SceneMemoryLimit(occupied);
            // Materials consume remaining scene capacity, not another copy
            // of the entire manual cap; DXGI headroom is checked again by
            // ShadowBindingWorker before resources are created.
            materialRequestBudgetBytes = totalAllowance > occupied ?
                totalAllowance - occupied : 0;
            materialRequestBudgetScene = scene;
        }
        if (materialRequestBudgetBytes)
            materialBindings.Request(*backend, scene, materialRequestBudgetBytes,
            uint64_t(settings.MaterialBatchMB) * 1024 * 1024, livePbrSource,
            OwnsNativeFeatures() && settings.NativePBR && settings.ImportLoosePBR,
            OwnsNativeFeatures() && settings.NativePBR && settings.ImportLoosePBR && settings.ImportArchivePBR,
            OwnsNativeFeatures() && settings.NativePBR && settings.ImportLoosePBR && settings.RuntimeOnlyMaterials);
    }
	materialBindings.Advance();
    if (auto completed = acceleration.GetScene(); completed &&
        completed->scope.scene == revision) {
        const auto matching = materialBindings.Get();
        if (matching && matching->scene == completed) {
            currentSceneStableAcceleration = completed;
            currentSceneStableBindings = matching;
        }
    }
    // Reuse the production worker's completed BLAS cache. The previous
    // production TLAS remains retained independently during rebuilding.
    if (settings.CaptureRuntimeGeometry && acceleration.Finished() &&
        currentSceneStableAcceleration &&
        currentSceneStableAcceleration->scope.scene == revision) {
        if (auto combined = runtimeGeometryPrepare.GetCurrentSceneCell();
            combined && combined->snapshot &&
            combined->snapshot->scope.scene == revision &&
            combined->snapshot->scope.policy == 2 &&
            combined->snapshot->scope.batch > currentSceneSubmittedBatch) {
            currentSceneSubmittedBatch = combined->snapshot->scope.batch;
            const uint64_t sceneAllowance = SceneMemoryLimit(combined->bytes +
                NativeAccelerationResidentBytes(currentSceneStableAcceleration));
            if (sceneAllowance > combined->bytes) {
                acceleration.Start(*backend, combined->snapshot, combined->geometries,
                    uint64_t(settings.BudgetMB) * DX12Native::VideoMemoryBudget::kMiB);
            }
        }
    }
	DispatchShadow("early-prepass");
}

void DX12NativeScene::DispatchShadow(const char* a_stage)
{
	Settings shadowSettings;
	{ std::scoped_lock lock(settingsMutex); shadowSettings = settings; }
	if (!shadowSettings.Enabled || !shadowSettings.ShadowMask || transitionOpen || globals::game::isVR || !backend || !globals::game::graphicsState ||
		!globals::shaderCache || !globals::shaderCache->IsEnabled() ||
		!globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) {
		shadowStatus = "Shadow mask unavailable for this render scope";
		return;
	}
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (shadow.Get(frame)) return;
	shadowStage = a_stage;
	if (!globals::game::frameBufferCachedValid || globals::game::frameBufferCachedFrame != frame) {
		shadowStatus = "Waiting for current-frame camera";
		return;
	}
	const auto& camera = globals::game::frameBufferCached.nonVR;
	const auto* worldCamera = RE::Main::WorldRootCamera();
	if (!worldCamera || !std::isfinite(camera.CameraProj._44) || std::abs(camera.CameraProj._44) > 1e-5f) {
		shadowStatus = "Main perspective camera unavailable";
		return;
	}
	const auto& origin = worldCamera->world.translate;
	const float dx = camera.CameraPosAdjust.x + camera.CameraViewInverse._14 - origin.x;
	const float dy = camera.CameraPosAdjust.y + camera.CameraViewInverse._24 - origin.y;
	const float dz = camera.CameraPosAdjust.z + camera.CameraViewInverse._34 - origin.z;
	const float errorSquared = dx * dx + dy * dy + dz * dz;
	if (!std::isfinite(errorSquared) || errorSquared > 0.25f) {
		shadowStatus = "Waiting for current main camera";
		return;
	}
	auto scene = GetPublishedScene();
	if (!scene) { shadowStatus = "Prepared scene or acceleration structure unavailable"; return; }
	const auto dynamicSize = Util::ConvertToDynamic(globals::state->screenSize);
	const std::array<uint32_t, 2> size{ uint32_t(dynamicSize.x), uint32_t(dynamicSize.y) };
	const auto nativeLights = GetNativeLLFLights(frame);
	const auto& preparedLights = scene->cell->nativeLights;
	const std::span<const DX12Native::PreparedPointLight> lights = nativeLights ?
		std::span<const DX12Native::PreparedPointLight>(nativeLights->lights.data(), nativeLights->count) :
		std::span<const DX12Native::PreparedPointLight>(preparedLights);
	if (lights.empty() || (shadowSettings.ShadowAllLights && lights.size() > 32) ||
		(!shadowSettings.ShadowAllLights && shadowSettings.ShadowLightIndex >= lights.size())) {
		shadowStatus = nativeLights ? "No current-frame NativeLLF lights selected" : "Selected prepared shadow lights unavailable or exceed 32";
		return;
	}
	const auto selectedLights = shadowSettings.ShadowAllLights ? lights : lights.subspan(shadowSettings.ShadowLightIndex, 1);
	const DX12Native::ShadowTraceControls controls{
		shadowSettings.ShadowReachMultiplier, shadowSettings.ShadowFalloffFraction,
		shadowSettings.ShadowFalloffExponent, shadowSettings.ShadowEmitterRadius,
		shadowSettings.ShadowStochastic, shadowSettings.ShadowDistanceFade, shadowSettings.ShadowNormalInfluence,
		shadowSettings.ShadowTraceDensityMode
	};
	shadow.Dispatch(*backend, scene->acceleration, scene->materials, selectedLights, camera, size, frame,
		uint64_t(shadowSettings.FrameBudgetMB) * 1024 * 1024, controls);
	shadowStatus = shadow.Status();
}

std::optional<DX12NativeScene::SceneFrame> DX12NativeScene::GetSceneFrame(uint32_t a_frame) const
{
	if (!loaded || !settings.Enabled || transitionOpen || !globals::game::graphicsState || globals::game::graphicsState->GetFrameCount() != a_frame)
		return std::nullopt;
	SceneFrame frame{ a_frame, raster.Get(a_frame), GetPublishedScene(), GetShadowMask(a_frame), GetNativeSCMFrame(a_frame) };
	if (!frame.raster && !frame.prepared && !frame.shadow) return std::nullopt;
	return frame;
}

std::shared_ptr<const DX12Native::ShadowFrame> DX12NativeScene::GetShadowMask(uint32_t a_frame) const
{
	if (!loaded || !settings.Enabled || !settings.ShadowMask || transitionOpen) return nullptr;
	auto output = shadow.Get(a_frame);
	auto scene = GetPublishedScene();
	return output && scene && output->scene == scene->acceleration ? output : nullptr;
}

void DX12NativeScene::OnOpaqueRasterReady(const OpaqueRasterInputs& a_inputs)
{
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	if (!settings.Enabled || transitionOpen || !backend || !globals::game::graphicsState || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) return;
	const std::array inputs{ a_inputs.litColor, a_inputs.litSpecular, a_inputs.albedo, a_inputs.normalRoughness,
		a_inputs.masks, a_inputs.masks2, a_inputs.reflectance, a_inputs.motion };
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const globals::FrameBuffer* camera = nullptr;
	if (!globals::game::isVR && globals::game::frameBufferCachedValid && globals::game::frameBufferCachedFrame == frame) {
		const auto& cached = globals::game::frameBufferCached.nonVR;
		const auto* worldCamera = RE::Main::WorldRootCamera();
		if (worldCamera && std::isfinite(cached.CameraProj._44) && std::abs(cached.CameraProj._44) <= 1e-5f) {
			const auto& origin = worldCamera->world.translate;
			const float dx = cached.CameraPosAdjust.x + cached.CameraViewInverse._14 - origin.x;
			const float dy = cached.CameraPosAdjust.y + cached.CameraViewInverse._24 - origin.y;
			const float dz = cached.CameraPosAdjust.z + cached.CameraViewInverse._34 - origin.z;
			const float errorSquared = dx * dx + dy * dy + dz * dz;
			if (std::isfinite(errorSquared) && errorSquared <= 0.25f) camera = &cached;
		}
	}
	raster.Capture(*backend, inputs, Util::GetCurrentSceneDepthSRV(false), camera, frame, uint64_t(settings.FrameBudgetMB) * 1024 * 1024);
	// Earlier shadow hooks may precede the current-frame main camera.
	// Retry only when the same-frame producer has not already published a mask;
	// DispatchShadow retains the camera, cell, material, scope and fence guards.
	if (settings.ShadowMask && !shadow.Get(frame)) {
		++shadowOpaqueRetryAttempts;
		DispatchShadow("opaque-raster-ready");
		if (shadow.Get(frame)) ++shadowOpaqueRetryRecovered;
	}
}

std::optional<DX12NativeScene::PublishedScene> DX12NativeScene::GetPublishedScene() const
{
	if (!loaded || !settings.Enabled || transitionOpen) return std::nullopt;
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	if (!cell || cell->GetFormID() != cellID) return std::nullopt;
	auto data = prepared.GetCell();
	auto scene = currentSceneStableAcceleration ? currentSceneStableAcceleration : acceleration.GetScene();
	if (!data || !scene || scene->scope.scene != revision) return std::nullopt;
	auto materials = currentSceneStableBindings ? currentSceneStableBindings : materialBindings.Get();
	if (materials && materials->scene != scene) materials.reset();
	return PublishedScene{ std::move(data), std::move(scene), std::move(materials), cellID, revision };
}

void DX12NativeScene::PublishDiagnostics()
{
	const auto bindings = materialBindings.Get();
	const auto retainedPages = DX12Native::RetainedMaterialPageBytes();
	json snapshot{ { "enabled", settings.Enabled }, { "nativeOwnership", OwnsNativeFeatures() },
		{ "nativePBR", settings.NativePBR }, { "nativeLLF", settings.NativeLLF }, { "nativeSCM", settings.NativeSCM },
		{ "nativeLightSource", OwnsNativeFeatures() ? "LLF-current-frame" : "prepared-cell" },
		{ "nativeSCMCasters", globals::game::graphicsState ?
			([&] { const auto snapshot = GetNativeSCMFrame(globals::game::graphicsState->GetFrameCount());
				return snapshot ? snapshot->casters.size() : size_t{ 0 }; })() : size_t{ 0 } },
		{ "nativeSCMMovers", globals::game::graphicsState ?
			([&] { const auto snapshot = GetNativeSCMFrame(globals::game::graphicsState->GetFrameCount());
				return snapshot ? std::ranges::count_if(snapshot->casters, [](const auto& caster) { return caster.dynamic; }) : std::ptrdiff_t{ 0 }; })() : std::ptrdiff_t{ 0 } },
		{ "nativeSCMRetiredStatic", globals::game::graphicsState ?
			([&] { const auto snapshot = GetNativeSCMFrame(globals::game::graphicsState->GetFrameCount());
				return snapshot ? snapshot->preparedStaticRetired : 0u; })() : 0u },
		{ "nativeLightCount", globals::game::graphicsState ?
			(GetNativeLLFLights(globals::game::graphicsState->GetFrameCount()).value_or(NativeLightBatch{}).count) : 0u }, { "cellID", cellID }, { "revision", revision }, { "backendFailure", failure },
		{ "raster", raster.GetDiagnostics() }, { "shadowStage", shadowStage }, { "shadowStatus", shadowStatus }, { "shadow", shadow.GetDiagnostics() },
		{ "shadowOpaqueRetryAttempts", shadowOpaqueRetryAttempts }, { "shadowOpaqueRetryRecovered", shadowOpaqueRetryRecovered },
		{ "shadowTraceControls", { { "revision", shadowControlsRevision },
			{ "allLights", settings.ShadowAllLights }, { "lightIndex", settings.ShadowLightIndex },
			{ "stochastic", settings.ShadowStochastic }, { "emitterRadius", settings.ShadowEmitterRadius },
			{ "reachMultiplier", settings.ShadowReachMultiplier }, { "falloffFraction", settings.ShadowFalloffFraction },
			{ "falloffExponent", settings.ShadowFalloffExponent }, { "distanceFade", settings.ShadowDistanceFade },
			{ "normalInfluence", settings.ShadowNormalInfluence }, { "traceDensityMode", settings.ShadowTraceDensityMode } } },
		{ "runtimeGeometry", { { "capturedMeshes", runtimeGeometry.Count() },
            { "capturedBytes", runtimeGeometry.Bytes() },
            { "alreadyPreparedInstances", runtimePreparedCoverage.load(std::memory_order_acquire) ?
                runtimePreparedCoverage.load(std::memory_order_acquire)->completed.size() : 0 },
            { "alreadyPreparedSkipped", runtimePreparedSkipped.load(std::memory_order_relaxed) },
            { "alreadyPreparedPruned", runtimePreparedPruned },
            { "currentSceneMerged", currentSceneStableAcceleration &&
                currentSceneStableAcceleration->scope.policy == 2 },
            { "currentSceneSubmittedBatch", currentSceneSubmittedBatch },
            { "currentSceneInputInstances", runtimeGeometryPrepare.GetCurrentSceneCell() &&
                runtimeGeometryPrepare.GetCurrentSceneCell()->snapshot ?
                runtimeGeometryPrepare.GetCurrentSceneCell()->snapshot->instances.size() : 0 },
            { "gpuBudget", { { "dxgiAvailable", runtimeGeometryBudgetStatus.dxgiAvailable },
                { "localBudgetBytes", runtimeGeometryBudgetStatus.localBudget },
                { "localUsageBytes", runtimeGeometryBudgetStatus.localUsage },
                { "reserveBytes", runtimeGeometryBudgetStatus.reserve },
                { "activeBaselineBytes", runtimeGeometryActiveBytes },
                { "runtimeAllowanceBytes", runtimeGeometryAllowedBytes } } },
            { "captureRevision", runtimeGeometry.Revision() },
            { "enabled", runtimeGeometryCaptureActive.load(std::memory_order_acquire) },
            { "preparing", runtimeGeometryPrepare.Busy() },
            { "batches", runtimeGeometryPrepare.Batches() },
            { "attempted", runtimeGeometryPrepare.Attempted() },
            { "failures", runtimeGeometryPrepare.Failures() },
            { "cacheHits", runtimeGeometryPrepare.CacheHits() },
            { "diskCache", {
                { "enabled", settings.CacheRuntimeGeometry },
                { "workerBusy", runtimeGeometryCache.Busy() },
                { "menuComplete", runtimeGeometryCache.MenuReady() },
                { "menuChecked", runtimeGeometryCache.MenuChecked() },
                { "menuAccepted", runtimeGeometryCache.MenuAccepted() },
                { "menuPreparedBytes", runtimeGeometryCache.MenuBytes() },
                { "stored", runtimeGeometryCache.Stored() },
                { "rejected", runtimeGeometryCache.Rejected() },
                { "failure", runtimeGeometryCache.Failure() } } },
            { "preparedInstances", runtimeGeometryPrepare.GetCell() && runtimeGeometryPrepare.GetCell()->snapshot ?
                runtimeGeometryPrepare.GetCell()->snapshot->instances.size() : 0 },
            { "preparedBytes", runtimeGeometryPrepare.GetCell() ?
                runtimeGeometryPrepare.GetCell()->bytes : 0 },
            { "activeSceneInstances", currentSceneStableAcceleration ?
                currentSceneStableAcceleration->instances.size() : 0 } } },
        { "preparedSceneBudget", {
            { "automatic", settings.BudgetMB == 0 },
            { "manualLimitBytes", uint64_t(settings.BudgetMB) * DX12Native::VideoMemoryBudget::kMiB },
            { "dxgiAvailable", preparedSceneBudgetStatus.dxgiAvailable },
            { "localBudgetBytes", preparedSceneBudgetStatus.localBudget },
            { "localUsageBytes", preparedSceneBudgetStatus.localUsage },
            { "reserveBytes", preparedSceneBudgetStatus.reserve },
            { "effectiveBytes", preparedSceneBudgetStatus.effectiveBytes }
        } },
        { "prepared", prepared.GetDiagnostics() }, { "materialFailure", materialBindings.Failure() }, { "materialUploadPending", materialBindings.UploadPending() },
        { "materialBindingsReady", bindings && bindings->scene == acceleration.GetScene() },
		{ "acceleration", acceleration.GetDiagnostics() }, { "published", GetPublishedScene().has_value() },
		{ "materialBudget", { { "dxgiAvailable", bindings ? bindings->materialBudget.dxgiAvailable : false },
			{ "selectedBatchLimitBytes", uint64_t(settings.MaterialBatchMB) * 1024 * 1024 },
            { "runtimeOnlyMaterialSources", OwnsNativeFeatures() && settings.NativePBR && settings.ImportLoosePBR &&
                settings.RuntimeOnlyMaterials },
			{ "loadedBatchLimitBytes", bindings ? bindings->uploadBatchBudget : 0 },
			{ "effectiveBytes", bindings ? bindings->materialBudget.effectiveBytes : 0 },
			{ "localBudgetBytes", bindings ? bindings->materialBudget.localBudget : 0 },
			{ "localUsageBytes", bindings ? bindings->materialBudget.localUsage : 0 },
			{ "textureBytes", bindings ? bindings->materials->bytes : 0 }, { "tableBytes", bindings ? bindings->materials->tableBytes : 0 },
			{ "retainedPageBytes", retainedPages ? json(*retainedPages) : json(nullptr) },
			{ "peakStagingBytes", bindings ? bindings->materials->peakStagingBytes : 0 },
			{ "uploadedBytes", bindings ? bindings->materials->uploadedBytes : 0 }, { "uploadedPages", bindings ? bindings->materials->uploadedPages : 0 },
			{ "reusedPages", bindings ? bindings->materials->reusedPages : 0 },
			{ "reusedTextures", bindings ? bindings->materials->reusedTextures : 0 },
			{ "diskCacheHits", bindings ? bindings->materials->diskCacheHits : 0 },
			{ "diskCacheWrites", bindings ? bindings->materials->diskCacheWrites : 0 },
			{ "asyncBatchPages", bindings ? bindings->materials->asyncBatchPages : 0 },
			{ "asyncBatchStagingBytes", bindings ? bindings->materials->asyncBatchStagingBytes : 0 },
			{ "readyMaterials", bindings ? bindings->materials->readyMaterials : 0 },
            { "rejectedMaterials", bindings ? bindings->materials->rejectedMaterials : 0 },
            { "textures", bindings ? bindings->materials->textures : 0 },
            { "livePbrMatched", bindings ? bindings->materials->livePbrMatched : 0 },
            { "livePbrApplied", bindings ? bindings->materials->livePbrApplied : 0 },
            { "livePbrEmissiveApplied", bindings ? bindings->materials->livePbrEmissiveApplied : 0 },
            { "livePbrUnsupported", bindings ? bindings->materials->livePbrUnsupported : 0 },
            { "livePbrRecipeIssueCount", bindings ? bindings->materials->liveRecipeIssues.affected : 0 },
            { "livePbrRecipeIssues", bindings ? json(bindings->materials->liveRecipeIssues.issues) : json(std::array<uint32_t, 13>{}) },
            { "livePbrFeatureFlags", bindings ? json(bindings->materials->liveRecipeIssues.pbrFlags) : json(std::array<uint32_t, 7>{}) },
            { "livePbrUnsafeOpacity", bindings ? bindings->materials->livePbrUnsafeOpacity : 0 },
            { "livePbrMissingBasis", bindings ? bindings->materials->livePbrMissingBasis : 0 },
            { "livePbrMissingBindings", bindings ? bindings->materials->livePbrMissingBindings : 0 },
            { "livePbrOfflineFallback", bindings ? bindings->materials->livePbrOfflineFallback : 0 },
            { "livePbrSourcesMissing", bindings ? bindings->materials->livePbrSourcesMissing : 0 },
            { "livePbrSourcesAmbiguous", bindings ? bindings->materials->livePbrSourcesAmbiguous : 0 },
            { "livePbrSourcesIncompatible", bindings ? bindings->materials->livePbrSourcesIncompatible : 0 },
            { "looseDdsPlanned", bindings ? bindings->materials->looseDdsPlanned : 0 },
            { "looseDdsImported", bindings ? bindings->materials->looseDdsImported : 0 },
            { "looseDdsRejected", bindings ? bindings->materials->looseDdsRejected : 0 },
            { "looseDdsDeferred", bindings ? bindings->materials->looseDdsDeferred : 0 },
            { "looseDdsPreviouslyAttempted", bindings ? bindings->materials->looseDdsPreviouslyAttempted : 0 },
            { "streamBatches", materialBindings.StreamBatches() },
            { "streamAttempted", materialBindings.StreamAttempted() },
            { "streamSuccessful", materialBindings.StreamSuccessful() },
            { "streamFailed", materialBindings.StreamFailed() },
            { "streamNextBatchPending", materialBindings.StreamWaitingForNextBatch() ? 1u : 0u },
            { "looseDdsReadyRecipes", bindings ? bindings->materials->looseDdsReadyRecipes : 0 },
            { "looseDdsFeatureLimitedRecipes", bindings ? bindings->materials->looseDdsFeatureLimitedRecipes : 0 },
            { "looseDdsSkippedRecipes", bindings ? bindings->materials->looseDdsSkippedRecipes : 0 },
            { "looseDdsPrepared", bindings ? bindings->materials->looseDdsPrepared : 0 },
            { "looseDdsInvalid", bindings ? bindings->materials->looseDdsInvalid : 0 },
            { "looseDdsBytes", bindings ? bindings->materials->looseDdsBytes : 0 },
            { "archiveDdsPlanned", bindings ? bindings->materials->archiveDdsPlanned : 0 },
            { "archiveDdsImported", bindings ? bindings->materials->archiveDdsImported : 0 },
            { "archiveDdsRejected", bindings ? bindings->materials->archiveDdsRejected : 0 },
            { "archiveDdsBytes", bindings ? bindings->materials->archiveDdsBytes : 0 },
            { "liveGpuUnobserved", bindings ? bindings->materials->liveGpuRoutes.unobserved : 0 },
            { "liveGpuUnsupported", bindings ? bindings->materials->liveGpuRoutes.unsupported : 0 },
            { "liveGpuCopyCandidates", bindings ? bindings->materials->liveGpuRoutes.copyCandidates : 0 },
            { "liveGpuSharedCandidates", bindings ? bindings->materials->liveGpuRoutes.sharedCandidates : 0 },
            { "livePbrAmbiguous", bindings ? bindings->materials->livePbrAmbiguous : 0 },
            { "livePbrIncompatible", bindings ? bindings->materials->livePbrIncompatible : 0 },
            { "livePbrMissing", bindings ? bindings->materials->livePbrMissing : 0 },
            { "rejectedTextureSources", bindings ? bindings->materials->rejectedTextureSources : 0 },
			{ "firstTextureSourceFailure", bindings ? bindings->materials->firstTextureSourceFailure : std::string{} },
			{ "uploadWaitMicroseconds", bindings ? bindings->materials->uploadWaitMicroseconds : 0 } } } };
	{
		std::scoped_lock lock(coverageMutex);
		snapshot["pbrCandidates"] = { { "pending", bool(coverageTask) }, { "failed", coverageFailed },
			{ "sceneRevision", coverageRevision }, { "currentScene", coverageResult && coverageRevision == revision },
			{ "unique", coverageResult ? coverageResult->uniqueCandidates : 0 }, { "ambiguous", coverageResult ? coverageResult->ambiguous : 0 },
			{ "recipeGenerationAvailable", coverageResult && coverageResult->recipeGenerationAvailable },
			{ "recipeRevision", coverageResult ? coverageResult->recipeRevision : 0 },
			{ "candidateRecipes", coverageResult ? coverageResult->candidateRecipes : 0 },
			{ "candidateRecipesMissing", coverageResult ? coverageResult->candidateRecipesMissing : 0 },
			{ "unobserved", coverageResult ? coverageResult->noObservation : 0 }, { "missingIdentity", coverageResult ? coverageResult->missingIdentity : 0 } };
	}
	std::scoped_lock lock(diagnosticsMutex);
	diagnostics = std::move(snapshot);
}

json DX12NativeScene::GetDiagnostics()
{
	std::scoped_lock lock(diagnosticsMutex);
	return diagnostics;
}

void DX12NativeScene::RegisterUxActions()
{
	FEATURE_COMMAND("analyzePBRCandidates", "Queue an analysis of unverified reference and shape candidates and available recipes; read pbrCandidates in diagnostics.", [](Feature* self, const json&) {
		static_cast<DX12NativeScene*>(self)->coverageRequested = true;
	});
	FEATURE_COMMAND("setShadowMask", "Queue the separate early prepared-scene RT shadow mask setting. Params: enabled (boolean).", [](Feature* self, const json& args) {
		auto& scene = *static_cast<DX12NativeScene*>(self);
		const bool enabled = args.at("enabled").get<bool>();
		scene.requestedShadow = enabled ? 1 : 0;
		if (enabled) scene.requestedEnabled = 1;
		scene.shadowResetRequested = true;
	});
	FEATURE_COMMAND("setShadowLayer", "Legacy compatibility: layer enabled/contribution/strength/blend/radiusPixels/receiverTolerance/preview/afterLighting forward to Native Composite; allLights/lightIndex/stochastic/emitterRadius/reachMultiplier/falloffFraction/falloffExponent/distanceFade/normalInfluence/traceDensityMode (0 Full, 1 Two-thirds, 2 Half, 3 One-third, 4 Quarter, 5 One-sixth, 6 One-ninth, 7 Four-ninths) configure raw DX12 shadow-mask tracing.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<DX12NativeScene*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		auto& s = feature.settings;
		const auto tune = [&](const char* key, float& target, float low, float high) {
			if (!args.contains(key)) return;
			const float v = args.at(key).get<float>();
			if (std::isfinite(v)) target = std::clamp(v, low, high);
		};
				tune("emitterRadius", s.ShadowEmitterRadius, 0.0f, 8.0f);
		tune("reachMultiplier", s.ShadowReachMultiplier, 0.1f, 64.0f);
		tune("falloffFraction", s.ShadowFalloffFraction, 0.02f, 1.0f);
		tune("falloffExponent", s.ShadowFalloffExponent, 0.25f, 4.0f);
		if (args.contains("allLights")) s.ShadowAllLights = args.at("allLights").get<bool>();
		if (args.contains("lightIndex")) s.ShadowLightIndex = args.at("lightIndex").get<uint32_t>();
		if (args.contains("stochastic")) s.ShadowStochastic = args.at("stochastic").get<bool>();
		if (args.contains("distanceFade")) s.ShadowDistanceFade = args.at("distanceFade").get<bool>();
		if (args.contains("normalInfluence")) s.ShadowNormalInfluence = std::min(args.at("normalInfluence").get<uint32_t>(), 2u);
		if (args.contains("traceDensityMode")) s.ShadowTraceDensityMode = std::min(args.at("traceDensityMode").get<uint32_t>(), 7u);
		++feature.shadowControlsRevision;
		globals::features::nativeComposite.ConfigureShadowLayer(args);
	});
	FEATURE_COMMAND("setPreparedScene", "Queue prepared scene settings. Params: enabled (boolean), budgetMB (optional integer; 0 = automatic DXGI, otherwise 64..4096), materialBatchMB (optional integer, 1..64).", [](Feature* self, const json& args) {
		auto& scene = *static_cast<DX12NativeScene*>(self);
		scene.requestedEnabled = args.at("enabled").get<bool>() ? 1 : 0;
		if (args.contains("budgetMB")) { const auto mb = args.at("budgetMB").get<uint32_t>(); scene.requestedBudget = mb ? std::clamp(mb, 64u, 4096u) : 0u; }
		if (args.contains("materialBatchMB")) scene.requestedMaterialBatch = DX12Native::ClampMaterialBatchMB(args.at("materialBatchMB").get<uint32_t>());
		scene.resetRequested = true;
	});
}
