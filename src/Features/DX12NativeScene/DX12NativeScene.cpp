#include "DX12NativeScene.h"
#include "Features/DX12NativeScene/CellPackedFile.h"
#include "RE/B/BSTriShape.h"
#include "RE/T/TESWorldSpace.h"
#include "Globals.h"
#include "TruePBR.h"
#include "Features/LightLimitFix.h"
#include "Features/DX12Resources/DX12Resources.h"
#include "Features/NativeFeatures.h"
#include <BS_thread_pool.hpp>
#include "Features/NativeComposite/NativeComposite.h"
#include "GpuPass.h"
#include "State.h"
#include "ShaderCache.h"
#include "Utils/DevBenchUx.h"
#include "Utils/D3D.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12NativeScene::Settings, Enabled, ShadowMask, NativePBR, NativeLLF, NativeSCM, BudgetMB, BudgetPolicyVersion, GlobalDXGIHeadroomPercent, SubdivisionMode, AutoGridMemoryScreening, SubcellSize, BuildCandidateGridTLAS, UseGridForNativeDXR, FrameBudgetMB, MaterialBatchMB, UseLivePBR, ImportLoosePBR, ImportArchivePBR, RuntimeOnlyMaterials, CaptureRuntimeGeometry, CaptureSCMRigidCasters, CacheRuntimeGeometry, ObserveNativeCharacters, CharacterPlayerOnly, CharacterStageGeometryDX12, CharacterRunSkinningDX12, CharacterValidateSkinningDX12, CharacterBuildIsolatedBLASDX12, CharacterBuildIsolatedTLASDX12, CharacterRunIsolatedRayQueryDX12, CharacterShadowCasterVisibility, CharacterMaxActors, CharacterObservationPeriod, CharacterBLASIntervalFrames, ShadowAllLights, ShadowStochastic, ShadowDistanceFade, ShadowLightIndex, ShadowNormalInfluence, ShadowEmitterRadius, ShadowReachMultiplier, ShadowFalloffFraction, ShadowFalloffExponent, ShadowTraceDensityMode)


namespace
{
    // Count each shared BLAS once; a combined scene must not double-count
    // memory merely because multiple TLAS instances refer to one bottom level.
    // Lightweight, no-decode preflight for already-cached full CELL packages.
    // This is an estimate of geometry + BLAS result/scratch + shading; DXGI
    // remains the authority for actual admission. No render-thread file parse.
    uint64_t EstimateCachedCellFootprint(const std::filesystem::path& directory)
    {
        std::error_code error;
        const auto geom = std::filesystem::file_size(directory / "cell-geometry.rsgp", error);
        if (error || geom < 16 || geom > DX12Packed::kMaximumPackBytes) return 0;
        error.clear();
        const auto shading = std::filesystem::file_size(directory / "cell-shading.rssp", error);
        if (error || shading < 16 || shading > DX12Packed::kMaximumPackBytes) return 0;
        constexpr uint64_t kFixedASAndDescriptorEstimate = 64ull * 1024 * 1024;
        return uint64_t(geom) * 3ull + uint64_t(shading) * 2ull +
            kFixedASAndDescriptorEstimate;
    }

    // One logical interior CELL or exterior WRLD owns the native scene.
    // Exterior CELL changes within a WRLD must not reset BLAS/light generations.
    struct NativeSpatialScope
    {
        DX12ResourceService::CellRequest request;
        std::filesystem::path directory;
        uint32_t formID = 0;
        bool exterior = false;
    };

    std::optional<NativeSpatialScope> ResolveNativeSpatialScope(
        RE::PlayerCharacter* player, RE::TESObjectCELL* cell)
    {
        if (!player || !cell) return std::nullopt;
        const bool exterior = cell->IsExteriorCell();
        auto* world = exterior ? player->GetWorldspace() : nullptr;
        if (exterior && !world) return std::nullopt;
        const auto* file = exterior ? world->GetFile(0) : cell->GetFile(0);
        const auto filename = file ? file->GetFilename() : std::string_view{};
        const uint32_t local = exterior ? world->GetLocalFormID() : cell->GetLocalFormID();
        const uint32_t id = exterior ? world->GetFormID() : cell->GetFormID();
        if (filename.empty() || !local || !id) return std::nullopt;
        NativeSpatialScope scope{};
        scope.request = { std::string(filename), local, exterior };
        scope.directory = std::filesystem::path(
            "Data/SKSE/Plugins/CommunityShaders/DX12-cache") /
            scope.request.CacheFamily() / scope.request.plugin / std::format("{:06X}", local);
        scope.formID = id;
        scope.exterior = exterior;
        return scope;
    }

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
	// Ownership is selected by the native scene backend, not the upscaler,
	// presentation path, frame generation, or per-effect validity checks.
	// Native Lighting and the independent shadow producer remain separately
	// runtime-toggleable consumers of this one backend decision.
	return loaded && settings.Enabled && !transitionOpen && backend.has_value() &&
		!globals::game::isVR;
}

bool DX12NativeScene::IsPreparedStaticCaster(RE::BSGeometry& geometry) const
{
	if (!globals::features::nativeSCM.IsActive() || geometry.GetGeometryRuntimeData().skinInstance)
		return false;
	// Coverage describes actual acceleration instances, never the offline
	// manifest alone. Unmatched static geometry remains in SCM's raster path.
	const auto coverage = runtimePreparedCoverage.load(std::memory_order_acquire);
	const auto published = GetPublishedScene();
	if (!coverage || coverage->sceneRevision != revision || !published ||
        runtimeCoverageSource != published->acceleration) return false;
	const auto* reference = geometry.GetUserData();
	if (!reference || reference->IsDynamicForm() || geometry.name.empty()) return false;
	const auto* file = reference->GetFile(0);
	if (!file) return false;
	return coverage->Contains(
		std::format("{:06X}:{}", reference->GetLocalFormID(), file->GetFilename()), geometry.name.c_str());
}

std::shared_ptr<const ShadowCasterManager::NativeCasterFrame> DX12NativeScene::GetNativeSCMFrame(uint32_t a_frame) const
{
	return globals::features::nativeSCM.IsActive() ?
		ShadowCasterManager::GetNativeCasterFrame(a_frame) : nullptr;
}

std::optional<DX12NativeScene::NativeLightBatch> DX12NativeScene::GetNativeLLFLights(uint32_t a_frame) const
{
	if (!OwnsNativeFeatures()) return std::nullopt;
	const auto scene = GetPublishedScene();
	if (!scene) return std::nullopt;
	const auto merged = globals::features::nativeLLF.BuildUnifiedLights(scene->cell->nativeLights, a_frame);
	NativeLightBatch batch{};
	batch.lights = merged.lights;
	batch.count = merged.count;
	batch.observed = merged.observed;
	batch.prepared = merged.prepared;
	batch.matched = merged.matched;
	batch.added = merged.added;
    batch.overflow = merged.overflow;
    // Native scene supplementary lighting has its own immutable generation.
    // Live LLF lights win when present; stable baked observations fill gaps.
    if (const auto baked = scene->bakedLights) {
        const auto distanceSquared = [](const float3& a, const float3& b) {
            const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
            return x*x + y*y + z*z;
        };
        for (const auto& entry : baked->candidates) {
            if (entry.consecutiveObservations < 4 ||
                a_frame < entry.lastSeenFrame ||
                a_frame - entry.lastSeenFrame > 30) continue;
            bool exists = false;
            for (uint32_t i = 0; i < batch.count; ++i) {
                if (!batch.lights[i].supported) continue;
                const float tolerance = std::clamp(entry.light.radius * 0.005f, 1.0f, 8.0f);
                if (distanceSquared(entry.light.position, batch.lights[i].position) <= tolerance*tolerance) {
                    exists = true;
                    break;
                }
            }
            if (exists) continue;
            if (batch.count >= batch.lights.size()) { ++batch.overflow; break; }
            batch.lights[batch.count++] = entry.light;
            ++batch.baked;
        }
    }
	batch.clustered = merged.clusteredLive;
	batch.omitted = merged.omitted;
	return batch;
}

void DX12NativeScene::DrawSettings()
{
	std::scoped_lock lock(settingsMutex);
	if (ImGui::Button("Analyze live PBR candidates")) coverageRequested = true;
    ImGui::TextDisabled("Native DX12 ownership: %s", OwnsNativeFeatures() ? "active" : "inactive");
    ImGui::TextDisabled("NativePBR, NativeLLF and NativeSCM are now independent feature entries.");
    ImGui::TextDisabled("Native feature ownership follows this DX12 backend, without readiness fallback.");
    ImGui::BeginDisabled(!globals::features::nativePBR.IsActive());
    if (ImGui::Checkbox("Experimental: import missing loose PBR DDS", &settings.ImportLoosePBR)) resetRequested = true;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!globals::features::nativePBR.IsActive() || !settings.ImportLoosePBR);
    if (ImGui::Checkbox("Experimental: use archive-backed DDS fallback", &settings.ImportArchivePBR))
        resetRequested = true;
    ImGui::EndDisabled();
    ImGui::TextDisabled("Worker-only; maximum 8 sources / 32 MiB combined per load.");
    ImGui::TextDisabled("Archive fallback runs only when the loose DDS is absent.");
    ImGui::BeginDisabled(!globals::features::nativePBR.IsActive() || !settings.ImportLoosePBR);
    ImGui::Checkbox("Experimental: runtime-only material sources (no atlas manifest)",
        &settings.RuntimeOnlyMaterials);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Prepared geometry is still required; unsupported materials can lack native shading.");
    if (ImGui::Checkbox("Experimental: capture runtime static geometry", &settings.CaptureRuntimeGeometry))
        runtimeGeometryCaptureActive.store(settings.CaptureRuntimeGeometry, std::memory_order_release);
    ImGui::TextDisabled("Copies CPU-backed static mesh bytes only; GPU-only/skinned/dynamic meshes are excluded.");
    if (ImGui::Checkbox("NativeSCM: capture uncovered rigid caster geometry", &settings.CaptureSCMRigidCasters))
        runtimeGeometryCaptureActive.store(settings.Enabled &&
            (settings.CaptureRuntimeGeometry || (settings.CaptureSCMRigidCasters &&
                globals::features::nativeSCM.IsActive())), std::memory_order_release);
    ImGui::TextDisabled("SCM supplies CPU geometry; native DX12 owns BLAS, RayQuery and shading.");
    ImGui::TextDisabled("Skinned, moving and GPU-only casters are not yet native-covered.");
    ImGui::SeparatorText("Native Characters (third-person; observation only)");
    if (ImGui::Checkbox("Observe third-person character meshes", &settings.ObserveNativeCharacters) &&
        !settings.ObserveNativeCharacters) nativeCharacters.Reset();
    if (ImGui::Checkbox("Observe player only", &settings.CharacterPlayerOnly))
        nativeCharacters.Reset();
    ImGui::BeginDisabled(!settings.ObserveNativeCharacters);
    if (ImGui::Checkbox("Stage owned character geometry in DX12 (experimental)",
        &settings.CharacterStageGeometryDX12)) nativeCharacters.Reset();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!settings.ObserveNativeCharacters ||
        !settings.CharacterStageGeometryDX12);
    if (ImGui::Checkbox("Dispatch isolated DX12 skinning test (no TLAS ownership)",
        &settings.CharacterRunSkinningDX12)) nativeCharacters.Reset();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!settings.ObserveNativeCharacters ||
        !settings.CharacterStageGeometryDX12 || !settings.CharacterRunSkinningDX12);
    if (ImGui::Checkbox("Validate deformed DX12 vertex buffer (readback)",
        &settings.CharacterValidateSkinningDX12)) nativeCharacters.Reset();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!settings.ObserveNativeCharacters ||
        !settings.CharacterStageGeometryDX12 ||
        !settings.CharacterRunSkinningDX12 ||
        !settings.CharacterValidateSkinningDX12);
    if (ImGui::Checkbox("Build isolated dynamic character BLAS (no TLAS)",
        &settings.CharacterBuildIsolatedBLASDX12)) nativeCharacters.Reset();
    int blasInterval = int(settings.CharacterBLASIntervalFrames);
    if (ImGui::SliderInt("Isolated BLAS update interval (frames)",
        &blasInterval, 1, 120))
        settings.CharacterBLASIntervalFrames = uint32_t(blasInterval);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!settings.ObserveNativeCharacters ||
        !settings.CharacterStageGeometryDX12 ||
        !settings.CharacterRunSkinningDX12 ||
        !settings.CharacterValidateSkinningDX12 ||
        !settings.CharacterBuildIsolatedBLASDX12);
    if (ImGui::Checkbox("Build isolated character TLAS (single instance)",
        &settings.CharacterBuildIsolatedTLASDX12)) nativeCharacters.Reset();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!settings.ObserveNativeCharacters ||
        !settings.CharacterStageGeometryDX12 ||
        !settings.CharacterRunSkinningDX12 ||
        !settings.CharacterValidateSkinningDX12 ||
        !settings.CharacterBuildIsolatedBLASDX12 ||
        !settings.CharacterBuildIsolatedTLASDX12);
    if (ImGui::Checkbox("Trace isolated character RayQuery (diagnostic only)",
        &settings.CharacterRunIsolatedRayQueryDX12)) nativeCharacters.Reset();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!settings.ObserveNativeCharacters ||
        !settings.CharacterStageGeometryDX12 ||
        !settings.CharacterRunSkinningDX12 ||
        !settings.CharacterValidateSkinningDX12 ||
        !settings.CharacterBuildIsolatedBLASDX12 ||
        !settings.CharacterBuildIsolatedTLASDX12 ||
        !settings.CharacterRunIsolatedRayQueryDX12 || !settings.ShadowMask);
    ImGui::Checkbox("Character shadows in native visibility (experimental)",
        &settings.CharacterShadowCasterVisibility);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Only secondary shadow rays use the character TLAS.");
    ImGui::TextDisabled("Default off; no character receiver/material ownership.");
    ImGui::TextDisabled(settings.CharacterPlayerOnly ?
        "Only the third-person player root is inspected." :
        "Includes third-person player, NPCs and creatures.");
    ImGui::TextDisabled("Isolated DX12 tests do not replace raster or native SCM ownership.");
    int maxActors = int(settings.CharacterMaxActors);
    if (ImGui::SliderInt("Maximum observed actors", &maxActors, 1, 32))
        settings.CharacterMaxActors = uint32_t(maxActors);
    int interval = int(settings.CharacterObservationPeriod);
    if (ImGui::SliderInt("Observation interval (frames)", &interval, 1, 120))
        settings.CharacterObservationPeriod = uint32_t(interval);
    const auto characterStats = nativeCharacters.GetDiagnostics();
    ImGui::Text("Actor samples: %u  Meshes: %u  Skinned: %u  Rigid: %u  Dynamic: %u",
        characterStats.value("actors", 0u), characterStats.value("parts", 0u),
        characterStats.value("skinned", 0u), characterStats.value("rigid", 0u),
        characterStats.value("dynamic", 0u));
    ImGui::TextDisabled("Character worker: %s, last sample frame %u",
        characterStats.value("workerBusy", false) ? "busy" : "idle",
        characterStats.value("generationFrame", 0u));
    ImGui::Text("Owned DX12 character bind buffers: %zu (%.2f MiB)",
        characterStats.value("nativeStagedMeshes", size_t{ 0 }),
        double(characterStats.value("nativeStagedBytes", uint64_t{ 0 })) / (1024.0 * 1024.0));
    ImGui::TextDisabled("Character mesh evictions: %u (actor/equipment identity changes)",
        characterStats.value("nativeEvictedMeshes", 0u));
    ImGui::TextDisabled("Skin bind vertices, indices and raw bone registers are DX12-staged.");
    ImGui::TextDisabled("Deformation is an isolated GPU experiment; BLAS and character rays remain disabled.");
    ImGui::Text("Bone palette: %u VS registers (captured frame %u)",
        characterStats.value("poseRegisters", 0u), characterStats.value("poseFrame", 0u));
    ImGui::TextDisabled("Pose stage deferrals: %u",
        characterStats.value("poseDeferrals", 0u));
    const auto poseNeeded = characterStats.value("poseRequiredBones", 0u);
    const auto poseProvided = characterStats.value("poseAvailableBones", 0u);
    ImGui::Text("Last skin-palette check: %u available / %u required bones",
        poseProvided, poseNeeded);
    const auto poseRejected = characterStats.value("posePaletteRejections", 0u);
    if (poseRejected)
        ImGui::TextDisabled("Incompatible palettes rejected: %u (no GPU dispatch)",
            poseRejected);
    ImGui::Text("Deformed output: %u vertices, submitted %s, GPU ready %s",
        characterStats.value("deformOutputVertices", 0u),
        characterStats.value("deformSubmitted", false) ? "yes" : "no",
        characterStats.value("deformGpuReady", false) ? "yes" : "no");
    ImGui::Text("Deformation sanity: %s, sampled %u, non-finite %u (pose frame %u)",
        characterStats.value("validationReady", false) ?
            (characterStats.value("validationSane", false) ? "finite" : "FAILED") : "pending/off",
        characterStats.value("validationInspected", 0u),
        characterStats.value("validationNonFinite", 0u),
        characterStats.value("validationFrame", 0u));
    if (characterStats.value("validationReady", false))
        ImGui::TextDisabled("Max displacement vs bind pose: %.2f game units",
            characterStats.value("validationMaximumBindDisplacement", 0.0f));
    ImGui::TextDisabled("Sanity checks do not prove Skyrim skinning parity.");
    ImGui::Text("Dynamic BLAS preflight: DXR %s, DXGI headroom %s",
        characterStats.value("blasPreflightDXR", false) ? "ready" : "unavailable",
        characterStats.value("blasPreflightBudget", false) ? "sufficient" : "insufficient/unknown");
    if (characterStats.value("blasPreflightReady", false)) {
        constexpr double mib = 1024.0 * 1024.0;
        ImGui::TextDisabled("BLAS result %.2f MiB, scratch %.2f MiB, update scratch %.2f MiB",
            double(characterStats.value("blasPreflightResultBytes", uint64_t{ 0 })) / mib,
            double(characterStats.value("blasPreflightScratchBytes", uint64_t{ 0 })) / mib,
            double(characterStats.value("blasPreflightUpdateScratchBytes", uint64_t{ 0 })) / mib);
        const auto preflightStatus =
            characterStats.value("blasPreflightStatus", std::string{});
        if (!preflightStatus.empty())
            ImGui::TextWrapped("Character BLAS plan: %s", preflightStatus.c_str());
    }
    ImGui::Text("Isolated character BLAS: %s, %s, pose frame %u",
        characterStats.value("isolatedBLASSubmitted", false) ?
            (characterStats.value("isolatedBLASReady", false) ? "GPU ready" : "submitted") :
            "not submitted",
        characterStats.value("isolatedBLASRefit", false) ? "refit" : "build",
        characterStats.value("isolatedBLASFrame", 0u));
    ImGui::TextDisabled("BLAS resources %.2f MiB, scratch %.2f MiB, pending %u",
        double(characterStats.value("isolatedBLASBytes", uint64_t{ 0 })) / (1024.0 * 1024.0),
        double(characterStats.value("isolatedBLASScratchBytes", uint64_t{ 0 })) /
            (1024.0 * 1024.0),
        characterStats.value("isolatedBLASPending", 0u));
    const auto isolatedBlasFailure =
        characterStats.value("isolatedBLASError", std::string{});
    if (!isolatedBlasFailure.empty())
        ImGui::TextWrapped("Isolated BLAS: %s", isolatedBlasFailure.c_str());
    ImGui::Text("Isolated character TLAS: %s, pose frame %u",
        characterStats.value("isolatedTLASSubmitted", false) ?
            (characterStats.value("isolatedTLASReady", false) ? "GPU ready" : "submitted") :
            "not submitted",
        characterStats.value("isolatedTLASFrame", 0u));
    ImGui::TextDisabled("TLAS %.2f MiB, pending %u",
        double(characterStats.value("isolatedTLASBytes", uint64_t{ 0 })) /
            (1024.0 * 1024.0),
        characterStats.value("isolatedTLASPending", 0u));
    const auto tlasFailure = characterStats.value("isolatedTLASError", std::string{});
    if (!tlasFailure.empty())
        ImGui::TextWrapped("Isolated TLAS: %s", tlasFailure.c_str());
    const auto rayStatus = characterStats.value("isolatedRayStatus", 0u);
    const char* rayState = rayStatus == 3u ? "triangle hit" :
        rayStatus == 2u ? "miss" :
        rayStatus == 1u ? "no valid triangle" : "not ready/invalid";
    ImGui::Text("Isolated RayQuery: %s; %s (pose frame %u)",
        characterStats.value("isolatedRayReady", false) ? "GPU ready" :
        characterStats.value("isolatedRaySubmitted", false) ? "submitted" : "not submitted",
        rayState, characterStats.value("isolatedRayFrame", 0u));
    if (rayStatus == 3u)
        ImGui::TextDisabled("Ray hit t %.4f; primitive %u; instance %u",
            characterStats.value("isolatedRayDistance", 0.0f),
            characterStats.value("isolatedRayPrimitive", UINT32_MAX),
            characterStats.value("isolatedRayInstance", UINT32_MAX));
    const auto rayError = characterStats.value("isolatedRayError", std::string{});
    if (!rayError.empty())
        ImGui::TextWrapped("Isolated RayQuery: %s", rayError.c_str());
    const auto casterReadiness = nativeCharacters.GetCasterReadiness(
        globals::game::graphicsState ?
            globals::game::graphicsState->GetFrameCount() : 0u);
    ImGui::TextWrapped("Character shadow eligibility: %s",
        casterReadiness.reason);
    ImGui::TextDisabled("World-bound plausibility is not exact pose parity.");
    const auto characterDeformError = characterStats.value("deformError", std::string{});
    if (!characterDeformError.empty())
        ImGui::TextWrapped("Deformation status: %s", characterDeformError.c_str());
    ImGui::TextDisabled("DXGI character deferrals: %u  invalid uploads: %u",
        characterStats.value("nativeDxgiDeferrals", 0u),
        characterStats.value("nativeUploadRejections", 0u));
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
		ImGui::TextDisabled("Receiver rays generated inside native DX12 RayQuery.");
        ImGui::TextDisabled("No separate camera-ray pipeline or DX11 compute dispatch.");
		changed |= ImGui::Checkbox("Shadow all unified native point lights", &settings.ShadowAllLights);
		if (!settings.ShadowAllLights) {
			const auto scene = GetPublishedScene();
			int index = int(settings.ShadowLightIndex);
			const auto unified = scene && globals::game::graphicsState ?
                GetNativeLLFLights(globals::game::graphicsState->GetFrameCount()) : std::nullopt;
            const int maxIndex = unified ? int(unified->count ? unified->count - 1 : 0) : 0;
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
    ImGui::SeparatorText("Global native DXGI budget policy");
    int globalReserve = int(settings.GlobalDXGIHeadroomPercent);
    if (ImGui::SliderInt("Global DXGI reserve (% of local budget)", &globalReserve, 1, 50)) {
        settings.GlobalDXGIHeadroomPercent = static_cast<uint32_t>(globalReserve);
        DX12Native::VideoMemoryBudget::SetHeadroomPercent(settings.GlobalDXGIHeadroomPercent);
    }
    ImGui::TextDisabled("Shared by native scene, materials, BLAS/TLAS, lighting, shadows and composite.");
    ImGui::TextDisabled("Reserve policy changes future admissions; current GPU allocations retire via their fences.");
    ImGui::SeparatorText("Native subdivision preparation (experimental)");
    const char* gridModes[]{ "Full scene", "Automatic (after scene estimate)", "Forced grid preparation" };
    int gridMode = static_cast<int>(settings.SubdivisionMode);
    if (ImGui::Combo("Subdivision preparation mode", &gridMode, gridModes, 3)) {
        settings.SubdivisionMode = static_cast<uint32_t>(gridMode);
        resetRequested.store(true, std::memory_order_release);
    }
    if (settings.SubdivisionMode == 1) {
        ImGui::Checkbox("Screen prepared scene size before automatic subdivision",
            &settings.AutoGridMemoryScreening);
        ImGui::TextDisabled("Off: automatic mode immediately selects the grid; on: DXGI-based scene screening.");
        ImGui::TextDisabled("An automatic grid decision stays latched until scene reset.");
    }
    int gridUnits = static_cast<int>(settings.SubcellSize / 1024);
    if (ImGui::SliderInt("Subcell width (x1024 units)", &gridUnits, 2, 16)) {
        settings.SubcellSize = uint32_t(gridUnits * 1024);
        resetRequested.store(true, std::memory_order_release);
    }
    ImGui::TextDisabled("Exteriors use WRLD-scoped 3x3 geometry; interiors keep CELL-scoped caches.");
    ImGui::TextDisabled("Grid TLAS publication remains experimental and opt-in.");
    ImGui::TextDisabled("Outdoor static geometry coverage is still provisional.");
    if (ImGui::Checkbox("Build isolated grid TLAS for diagnostic testing", &settings.BuildCandidateGridTLAS))
        resetRequested.store(true, std::memory_order_release);
    ImGui::BeginDisabled(!settings.BuildCandidateGridTLAS);
    if (ImGui::Checkbox("Experimental: publish ready grid to native DXR", &settings.UseGridForNativeDXR))
        resetRequested.store(true, std::memory_order_release);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Opt-in: complete TLAS + material pair swaps together, with no blank frame.");
    ImGui::TextDisabled("During grid rebuilds, the last published TLAS remains active.");
    ImGui::TextDisabled("Otherwise preserve the original full-scene renderer.");
    ImGui::TextDisabled("Forced + live grid skips expensive full-cell GPU preparation.");
    {
        const auto grid = subcellManager.GetDiagnostics();
        const auto candidate = candidateGridAcceleration.GetScene();
        const auto bindings = candidateGridBindings.Get();
        const auto active = GetPublishedScene();
        ImGui::Text("Grid center: (%lld, %lld) | loaded %u/9 | loading %u",
            static_cast<long long>(grid.value("centerX", int64_t{0})),
            static_cast<long long>(grid.value("centerY", int64_t{0})),
            grid.value("resident", 0u), grid.value("loading", 0u));
        ImGui::Text("Grid CPU snapshot: %zu instances | %llu MiB",
            static_cast<size_t>(grid.value("candidateInstances", uint64_t{0})),
            static_cast<unsigned long long>(grid.value("candidateBytes", uint64_t{0}) /
                DX12Native::VideoMemoryBudget::kMiB));
        ImGui::Text("TLAS: %s | materials: %s | native DXR: %s",
            candidate && candidate->scope.policy == 3 ? "ready" : "pending",
            candidate && bindings && bindings->scene == candidate ? "ready" : "pending",
            active && active->acceleration && active->acceleration->scope.policy == 3 ?
                "active grid" : "full scene / raster fallback");
    }
    ImGui::SeparatorText("Dynamic DXGI VRAM budgeting");
    ImGui::TextDisabled("Scene and frame-pool memory follow live DXGI; no manual ceilings.");
    const auto budgetInfo = GetDiagnostics().value("preparedSceneBudget", json::object());
    ImGui::Text("DXGI local usage: %.1f / %.1f MiB | effective scene allowance: %.1f MiB",
        double(budgetInfo.value("localUsageBytes", uint64_t{0})) / (1024.0 * 1024.0),
        double(budgetInfo.value("localBudgetBytes", uint64_t{0})) / (1024.0 * 1024.0),
        double(budgetInfo.value("effectiveBytes", uint64_t{0})) / (1024.0 * 1024.0));
    ImGui::TextDisabled("Prepared geometry, BLAS/TLAS, frames and materials honor the shared DXGI reserve.");
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
	ImGui::TextDisabled("Native frame-pool budgeting: automatic DXGI only.");
	ImGui::TextDisabled("Pool allocations respect DXGI headroom; independent of the prepared-scene budget.");
	const auto requestedBatch = requestedMaterialBatch.load();
	int batchMB = int(requestedBatch ? requestedBatch : settings.MaterialBatchMB);
	if (ImGui::SliderInt("Prepared material batch limit (MiB)", &batchMB, 1, 64)) requestedMaterialBatch = uint32_t(batchMB);
	ImGui::TextDisabled("Applies to the next prepared material load; larger sets use the existing worker path.");
}

void DX12NativeScene::LoadSettings(json& a_json)
{
	const Settings selected = a_json;
    // Stored user-selected memory ceilings are retired; only DXGI is authoritative.
    const auto reservePercent = std::clamp(selected.GlobalDXGIHeadroomPercent, 1u, 50u);
    DX12Native::VideoMemoryBudget::SetHeadroomPercent(reservePercent);
	// A compositor requesting the independent layer takes precedence over the
	// scene configuration's possibly older disabled producer state.
	if (requestedEnabled.load() != 1) requestedEnabled = selected.Enabled ? 1 : 0;
	if (requestedShadow.load() != 1) requestedShadow = selected.ShadowMask ? 1 : 0;
	requestedBudget = 0u;
	requestedFrameBudget = 0u;
	requestedMaterialBatch = DX12Native::ClampMaterialBatchMB(selected.MaterialBatchMB);
	std::scoped_lock lock(settingsMutex);
	const auto clamp = [](float value, float fallback, float low, float high) { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; };
	++shadowControlsRevision;
	settings.UseLivePBR = selected.UseLivePBR;
	settings.NativePBR = selected.NativePBR;
	settings.NativeLLF = selected.NativeLLF;
	settings.NativeSCM = selected.NativeSCM;
    settings.CaptureSCMRigidCasters = selected.CaptureSCMRigidCasters;
    settings.ObserveNativeCharacters = selected.ObserveNativeCharacters;
    if (settings.CharacterPlayerOnly != selected.CharacterPlayerOnly)
        nativeCharacters.Reset();
    settings.CharacterPlayerOnly = selected.CharacterPlayerOnly;
    if (settings.CharacterStageGeometryDX12 != selected.CharacterStageGeometryDX12)
        nativeCharacters.Reset();
    settings.CharacterStageGeometryDX12 = selected.CharacterStageGeometryDX12;
    if (settings.CharacterRunSkinningDX12 != selected.CharacterRunSkinningDX12)
        nativeCharacters.Reset();
    settings.CharacterRunSkinningDX12 = selected.CharacterRunSkinningDX12;
    if (settings.CharacterValidateSkinningDX12 != selected.CharacterValidateSkinningDX12)
        nativeCharacters.Reset();
    settings.CharacterValidateSkinningDX12 = selected.CharacterValidateSkinningDX12;
    if (settings.CharacterBuildIsolatedBLASDX12 != selected.CharacterBuildIsolatedBLASDX12)
        nativeCharacters.Reset();
    settings.CharacterBuildIsolatedBLASDX12 = selected.CharacterBuildIsolatedBLASDX12;
    if (settings.CharacterBuildIsolatedTLASDX12 != selected.CharacterBuildIsolatedTLASDX12)
        nativeCharacters.Reset();
    settings.CharacterBuildIsolatedTLASDX12 = selected.CharacterBuildIsolatedTLASDX12;
    if (settings.CharacterRunIsolatedRayQueryDX12 != selected.CharacterRunIsolatedRayQueryDX12)
        nativeCharacters.Reset();
    settings.CharacterRunIsolatedRayQueryDX12 = selected.CharacterRunIsolatedRayQueryDX12;
    settings.CharacterShadowCasterVisibility = selected.CharacterShadowCasterVisibility;
    settings.CharacterBLASIntervalFrames =
        std::clamp(selected.CharacterBLASIntervalFrames, 1u, 120u);
    settings.CharacterMaxActors = std::clamp(selected.CharacterMaxActors, 1u, 32u);
    settings.CharacterObservationPeriod = std::clamp(selected.CharacterObservationPeriod, 1u, 120u);
    settings.GlobalDXGIHeadroomPercent = reservePercent;
    settings.SubdivisionMode = std::min(selected.SubdivisionMode, 2u);
    settings.AutoGridMemoryScreening = selected.AutoGridMemoryScreening;
    settings.SubcellSize = std::clamp(selected.SubcellSize / 1024, 2u, 16u) * 1024u;
    settings.BuildCandidateGridTLAS = selected.BuildCandidateGridTLAS;
    settings.UseGridForNativeDXR = selected.UseGridForNativeDXR;
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
	selected.BudgetMB = 0;
	selected.FrameBudgetMB = 0;
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
    DX12Native::VideoMemoryBudget::SetHeadroomPercent(DX12Native::VideoMemoryBudget::kDefaultHeadroomPercent);
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


void DX12NativeScene::ObserveSCMRigidCaster(RE::BSGeometry& geometry)
{
    // Acquire only stable CPU-backed mesh inputs in the already-active SCM
    // cull pass. No raster lighting calculation is performed by this bridge.
    // Actual RT coverage is granted only by the published TLAS.
    if (!runtimeGeometryCaptureActive.load(std::memory_order_acquire) ||
        !settings.CaptureSCMRigidCasters || geometry.GetGeometryRuntimeData().skinInstance ||
        geometry.name.empty()) return;
    auto* reference = geometry.GetUserData();
    if (!reference || reference->IsDynamicForm()) return;
    const auto* source = reference->GetFile(0);
    if (!source || source->GetFilename().empty()) return;
    ObserveRuntimeGeometry(&geometry,
        std::format("{:06X}:{}", reference->GetLocalFormID(), source->GetFilename()),
        geometry.name.c_str());
}

void DX12NativeScene::ObserveRuntimeGeometry(RE::BSGeometry* geometry,
    const std::string& reference, const std::string& shape)
{
    if (!runtimeGeometryCaptureActive.load(std::memory_order_relaxed) || !geometry) return;
    if (const auto covered = runtimePreparedCoverage.load(std::memory_order_acquire);
        covered && covered->Contains(reference, shape)) {
        runtimePreparedSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A full mailbox or already-observed identity must not trigger renderer
    // vertex/index validation, per-frame copy budgeting or CPU acquisition.
    if (!runtimeGeometry.CanCapture(reference, shape)) return;
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
    if (length > 256 * 1024 ||
        !runtimeGeometry.CanCapture(reference, shape, length)) return;
    // Bound render-thread copy latency AND reject packets that cannot fit
    // before scanning or copying CPU vertex/index arrays.
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
    if (!backend) return 0;
    preparedSceneBudgetStatus = preparedSceneVideoMemory.Query(
        backend->d3d12Device.get(), trackedBytes, 0);
    return preparedSceneBudgetStatus.effectiveBytes;
}

void DX12NativeScene::ResetScene()
{
    // NativeLLF generations are cell-scoped. Do not retain old counts or
    // shadow-slot identities after new cells, loading screens or scene resets.
    globals::features::nativeLLF.ClearCapturedLights();
    nativeCharacters.Reset();
    subcellManager.Reset();
    candidateGridAcceleration.Reset();
    candidateGridBindings.Reset();
    requestedCandidateMaterialScene.reset();
    candidateMaterialAllowance = 0;
    lastSubmittedGridCell.reset();
    stableGridPublished.reset();
    stableGridCenter = {};
    autoGridDecisionLatched = false;
    hasPreviousPlayer = false;
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
    runtimeCoverageTask.reset(); // Worker owns its immutable TLAS while running.
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

// Completed TLAS identity generation is built away from Skyrim's render
// callbacks. Only the exact published TLAS may suppress legacy SCM casters.
struct DX12NativeScene::NativeSCMCoverageTask
{
    std::atomic<bool> done{ false };
    uint64_t sceneRevision = 0;
    std::shared_ptr<const DX12Native::TopLevel> source;
    std::shared_ptr<const DX12Native::PreparedRuntimeCoverage> result;
};

void DX12NativeScene::AdvanceNativeSCMCoverage()
{
    const auto published = GetPublishedScene();
    const auto candidate = published ? published->acceleration : acceleration.GetScene();
    if (runtimeCoverageTask && runtimeCoverageTask->done.load(std::memory_order_acquire)) {
        auto completed = std::move(runtimeCoverageTask);
        if (completed->result && completed->sceneRevision == revision &&
            completed->source == candidate &&
            candidate && candidate->scope.scene == revision) {
            // No publication until the worker's identity index and the
            // currently offered TLAS are exactly the same generation.
            runtimeCoverageSource = candidate;
            runtimePreparedCoverage.store(completed->result, std::memory_order_release);
            runtimePreparedPruned += runtimeGeometry.RemoveCovered(*completed->result);
        }
    }
    if (runtimeCoverageTask || !candidate || candidate->scope.scene != revision ||
        candidate == runtimeCoverageSource) return;
    try {
        auto work = std::make_shared<NativeSCMCoverageTask>();
        work->sceneRevision = revision;
        work->source = candidate;
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([work] {
            try {
                auto covered = std::make_shared<DX12Native::PreparedRuntimeCoverage>();
                covered->sceneRevision = work->sceneRevision;
                covered->completed.reserve(work->source->instances.size());
                for (const auto& instance : work->source->instances) {
                    const auto& identity = instance.preparedIdentity;
                    if (!identity.referenceKey.empty() && !identity.shapeName.empty())
                        covered->completed.insert(identity.referenceKey + '\0' + identity.shapeName);
                }
                work->result = std::move(covered);
            } catch (...) { /* Keep raster fallback; retry without a GPU/CPU wait. */ }
            work->done.store(true, std::memory_order_release);
        });
        runtimeCoverageTask = std::move(work);
    } catch (...) { /* Native coverage may not suppress the original SCM. */ }
}

void DX12NativeScene::EarlyPrepass()
{
	CS_GPU_PASS("DX12NativeScene::PreparedSceneProgress");
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	AdvanceCoverage();
	DX12Native::RetireMaterialUploads();
    // DataLoaded starts the opt-in disk flush before any native CELL/WRLD
    // material or geometry files may be requested. Never load stale cache
    // data while deletion is in progress or failed; raster stays available.
    if (!globals::features::dx12Resources.DiskGeometryCacheReady()) return;
	{
		std::scoped_lock lock(settingsMutex);
		if (const auto enabled = requestedEnabled.exchange(-1); enabled >= 0) settings.Enabled = enabled != 0;
		if (const auto shadowEnabled = requestedShadow.exchange(-1); shadowEnabled >= 0) settings.ShadowMask = shadowEnabled != 0;
		requestedBudget.store(UINT32_MAX, std::memory_order_relaxed);
		requestedFrameBudget.store(UINT32_MAX, std::memory_order_relaxed);
        settings.BudgetMB = 0;
        settings.FrameBudgetMB = 0;
		if (const auto batch = requestedMaterialBatch.exchange(0); batch) settings.MaterialBatchMB = batch;
	}
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
    const auto spatialScope = ResolveNativeSpatialScope(globals::game::player, cell);
    const auto currentID = spatialScope ? spatialScope->formID : 0;
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
	const bool captureNativeSCM = settings.CaptureSCMRigidCasters &&
        globals::features::nativeSCM.IsActive();
    runtimeGeometryCaptureActive.store(settings.Enabled &&
        (settings.CaptureRuntimeGeometry || captureNativeSCM),
        std::memory_order_release);
    if (resetRequested.exchange(false)) ResetScene();
    // Interior requests use CELL identity; exterior requests retain WRLD
    // identity across changing exterior CELLs. Companion runs asynchronously.
    if (settings.Enabled && !transitionOpen && spatialScope) {
        const auto position = globals::game::player->GetPosition();
        const auto estimate = spatialScope->exterior ? 0 :
            EstimateCachedCellFootprint(spatialScope->directory);
        if (backend) SceneMemoryLimit();
        // A scene-size decision is made once per worldspace/scene generation;
        // transient DXGI usage fluctuations must not toggle between renderers.
        if (settings.SubdivisionMode == 1 && (!settings.AutoGridMemoryScreening ||
            (estimate && preparedSceneBudgetStatus.dxgiAvailable &&
                estimate > preparedSceneBudgetStatus.effectiveBytes)))
            autoGridDecisionLatched = true;
        const bool grid = (spatialScope->exterior && settings.SubdivisionMode != 0) ||
            settings.SubdivisionMode == 2 ||
            (settings.SubdivisionMode == 1 && autoGridDecisionLatched);
        if (grid) {
            const double dx = hasPreviousPlayer ? position.x - previousPlayerX : 0.0;
            const double dy = hasPreviousPlayer ? position.y - previousPlayerY : 0.0;
            globals::features::dx12Resources.AdvanceStreamingScene(spatialScope->request,
                settings.SubcellSize, position.x, position.y, dx, dy);
        } else if (!spatialScope->exterior)
            globals::features::dx12Resources.AdvanceAutomaticInterior(spatialScope->request);
        previousPlayerX = position.x;
        previousPlayerY = position.y;
        hasPreviousPlayer = true;
    }
    // This CPU-only cache scheduler operates before the native scene/cell
    // early returns, so the menu can prewarm cached meshes without a TLAS.
    // All disk I/O and decoding remain on its background worker.
    if (settings.CacheRuntimeGeometry)
        runtimeGeometryCache.Advance(!cell && !transitionOpen,
            (settings.CaptureRuntimeGeometry || captureNativeSCM) && cell && !transitionOpen &&
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
    if (!spatialScope || (spatialScope->exterior && settings.SubdivisionMode == 0)) {
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
    if ((settings.CaptureRuntimeGeometry || captureNativeSCM) && currentSceneStableAcceleration &&
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
    // Prepare independently resident subcell GPU buffers and optional
    // diagnostic TLAS without changing the authoritative live renderer.
    if (settings.SubdivisionMode != 0) {
        const auto estimated = spatialScope->exterior ? 0 :
            EstimateCachedCellFootprint(spatialScope->directory);
        if (settings.SubdivisionMode == 1 && (!settings.AutoGridMemoryScreening ||
            (estimated && preparedSceneBudgetStatus.dxgiAvailable &&
                estimated > preparedSceneBudgetStatus.effectiveBytes)))
            autoGridDecisionLatched = true;
        const bool gridSelected = spatialScope->exterior ||
            settings.SubdivisionMode == 2 ||
            (settings.SubdivisionMode == 1 && autoGridDecisionLatched);
        if (gridSelected) {
            const auto& position = globals::game::player->GetPosition();
            subcellManager.Advance(*backend, spatialScope->directory,
                spatialScope->request.Key(), revision, settings.SubcellSize,
                position.x, position.y, 0.0, 0.0,
                0ull);
            if (settings.BuildCandidateGridTLAS) {
                const auto candidate = subcellManager.GetCombined();
                if (candidate && candidate->snapshot && candidate != lastSubmittedGridCell &&
                    (candidateGridAcceleration.Finished() || !lastSubmittedGridCell)) {
                    candidateGridAcceleration.Start(*backend, candidate->snapshot, candidate->geometries,
                        0ull);
                    lastSubmittedGridCell = candidate;
                }
                candidateGridAcceleration.Advance();
                // Canary path also builds a matching descriptor/material table.
                // Never publish the TLAS alone: Native Lighting and DXR shadow
                // queries need an exact matching binding set.
                const auto candidateScene = candidateGridAcceleration.GetScene();
                if (candidateScene && candidateScene->scope.scene == revision &&
                    candidateScene->scope.policy == 3) {
                    if (requestedCandidateMaterialScene != candidateScene) {
                        requestedCandidateMaterialScene = candidateScene;
                        const uint64_t occupied =
                            (lastSubmittedGridCell ? lastSubmittedGridCell->bytes : 0) +
                            NativeAccelerationResidentBytes(candidateScene);
                        const uint64_t allowance = SceneMemoryLimit(occupied);
                        candidateMaterialAllowance = allowance > occupied ? allowance - occupied : 0;
                    }
                    if (candidateMaterialAllowance) {
                        candidateGridBindings.Request(*backend, candidateScene,
                            candidateMaterialAllowance,
                            uint64_t(settings.MaterialBatchMB) * DX12Native::VideoMemoryBudget::kMiB,
                            {}, false, false, false);
                    }
                }
                candidateGridBindings.Advance();
                // Never expose a half-ready generation: promote the complete
                // source/TLAS/material triple at once. Keep the last valid
                // triple alive while new geometry and descriptors build.
                const auto readyCell = subcellManager.GetCombined();
                const auto readyTLAS = candidateGridAcceleration.GetScene();
                const auto readyBindings = candidateGridBindings.Get();
                const auto progress = subcellManager.GetDiagnostics();
                const auto center = DX12Native::SpatialSubcells::FromPosition(
                    position.x, position.y, settings.SubcellSize);
                if (readyCell && readyCell == lastSubmittedGridCell &&
                    readyCell->snapshot && readyTLAS && readyBindings &&
                    readyTLAS->scope.scene == revision &&
                    readyTLAS->scope.policy == 3 &&
                    readyTLAS->scope == readyCell->snapshot->scope &&
                    readyBindings->scene == readyTLAS &&
                    progress.value("centerX", int64_t{INT64_MIN}) == center.x &&
                    progress.value("centerY", int64_t{INT64_MIN}) == center.y &&
                    (!stableGridPublished ||
                        stableGridPublished->acceleration != readyTLAS ||
                        stableGridPublished->materials != readyBindings)) {
                    stableGridPublished = PublishedScene{
                        readyCell, readyTLAS, readyBindings, cellID, revision };
                    stableGridCenter = center;
                    ++gridHandoffs;
                }
            } else if (lastSubmittedGridCell) {
                candidateGridAcceleration.Reset();
                candidateGridBindings.Reset();
                requestedCandidateMaterialScene.reset();
                candidateMaterialAllowance = 0;
                lastSubmittedGridCell.reset();
            }
        }
    }
    // Forced streaming with an explicitly enabled live-grid canary must
    // NOT upload the entire CELL merely to decide where to subdivide.
    // Raster continues while partial grid assets become available.
    // In diagnostic/auto mode the established complete-scene renderer
    // remains the normal fallback.
    const auto projected = spatialScope->exterior ? 0 :
        EstimateCachedCellFootprint(spatialScope->directory);
    if (settings.SubdivisionMode == 1 && (!settings.AutoGridMemoryScreening ||
        (projected && preparedSceneBudgetStatus.dxgiAvailable &&
            projected > preparedSceneBudgetStatus.effectiveBytes)))
        autoGridDecisionLatched = true;
    const bool autoGrid = settings.SubdivisionMode == 1 && autoGridDecisionLatched;
    // No full-WRLD package exists. Exteriors must never load a single
    // exterior CELL and accidentally claim it covers the 3x3 worldspace.
    const bool exclusiveGrid = spatialScope->exterior ||
        ((settings.SubdivisionMode == 2 || autoGrid) &&
            settings.BuildCandidateGridTLAS && settings.UseGridForNativeDXR);
	if (!exclusiveGrid && !prepared.Requested()) {
		const auto* source = cell->GetFile(0);
		if (!source) { failure = "Cell source unavailable"; return; }
		const auto local = cell->GetLocalFormID();
		const auto key = std::format("{:06X}:{}", local, source->GetFilename());
		const auto directory = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/DX12-cache/cells") / source->GetFilename() / std::format("{:06X}", local);
        const uint64_t manualBytes = 0; // DXGI only.
        SceneMemoryLimit();
		prepared.Request(*backend, directory, key, revision, manualBytes, manualBytes);
	}
	if (!exclusiveGrid && !submitted) {
		if (auto scene = prepared.GetCell()) {
            const uint64_t sceneAllowance = SceneMemoryLimit(scene->bytes);
            if (sceneAllowance > scene->bytes) {
                acceleration.Start(*backend, scene->snapshot, scene->geometries,
                    0ull);
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
    // Match NativeSCM's static-caster coverage to the scene that Native DXR
    // actually exposes. When a partial grid becomes active, full-CELL TLAS
    // membership must no longer suppress raster fallback for absent casters.
    AdvanceNativeSCMCoverage();
    if (auto scene = acceleration.GetScene(); scene &&
        shadow.RetireScenesExcept(currentSceneStableAcceleration ? currentSceneStableAcceleration : scene) &&
        retiredMaterials.expired()) {
        if (globals::features::nativePBR.IsActive() && globals::game::graphicsState) {
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
            globals::features::nativePBR.IsActive() && settings.ImportLoosePBR,
            globals::features::nativePBR.IsActive() && settings.ImportLoosePBR && settings.ImportArchivePBR,
            globals::features::nativePBR.IsActive() && settings.ImportLoosePBR && settings.RuntimeOnlyMaterials);
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
    if ((settings.CaptureRuntimeGeometry || captureNativeSCM) && acceleration.Finished() &&
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
                    0ull);
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
	if (lights.empty() || (shadowSettings.ShadowAllLights && lights.size() > DX12Native::kMaximumNativePointLights) ||
		(!shadowSettings.ShadowAllLights && shadowSettings.ShadowLightIndex >= lights.size())) {
		shadowStatus = "Selected native shadow lights unavailable or exceed 64";
		return;
	}
	const auto selectedLights = shadowSettings.ShadowAllLights ? lights : lights.subspan(shadowSettings.ShadowLightIndex, 1);
	const DX12Native::ShadowTraceControls controls{
		shadowSettings.ShadowReachMultiplier, shadowSettings.ShadowFalloffFraction,
		shadowSettings.ShadowFalloffExponent, shadowSettings.ShadowEmitterRadius,
		shadowSettings.ShadowStochastic, shadowSettings.ShadowDistanceFade, shadowSettings.ShadowNormalInfluence,
		shadowSettings.ShadowTraceDensityMode
	};
    const bool requestCharacterCaster =
        shadowSettings.CharacterShadowCasterVisibility &&
        shadowSettings.ObserveNativeCharacters &&
        shadowSettings.CharacterStageGeometryDX12 &&
        shadowSettings.CharacterRunSkinningDX12 &&
        shadowSettings.CharacterValidateSkinningDX12 &&
        shadowSettings.CharacterBuildIsolatedBLASDX12 &&
        shadowSettings.CharacterBuildIsolatedTLASDX12 &&
        shadowSettings.CharacterRunIsolatedRayQueryDX12;
    auto characterCaster = requestCharacterCaster ?
        nativeCharacters.GetCasterReadiness(frame).tlas : nullptr;
	shadow.Dispatch(*backend, scene->acceleration, scene->materials, selectedLights, camera, size, frame,
		0ull, controls, std::move(characterCaster));
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
    // Copy only the character controls while holding the same mutex used
    // by the developer UI. Mesh acquisition runs without a settings lock.
    bool observeCharacters = false, playerOnly = false;
    bool stageCharacterGeometry = false, runCharacterSkinning = false;
    bool validateCharacterSkinning = false, buildIsolatedCharacterBLAS = false;
    bool buildIsolatedCharacterTLAS = false, traceIsolatedCharacterRay = false;
    uint32_t actorLimit = 16, observationInterval = 8, blasUpdateInterval = 8;
    {
        std::scoped_lock lock(settingsMutex);
        observeCharacters = settings.ObserveNativeCharacters;
        playerOnly = settings.CharacterPlayerOnly;
        stageCharacterGeometry = settings.CharacterStageGeometryDX12;
        runCharacterSkinning = settings.CharacterRunSkinningDX12;
        validateCharacterSkinning = settings.CharacterValidateSkinningDX12;
        buildIsolatedCharacterBLAS = settings.CharacterBuildIsolatedBLASDX12;
        buildIsolatedCharacterTLAS = settings.CharacterBuildIsolatedTLASDX12;
        traceIsolatedCharacterRay = settings.CharacterRunIsolatedRayQueryDX12;
        actorLimit = settings.CharacterMaxActors;
        observationInterval = settings.CharacterObservationPeriod;
        blasUpdateInterval = settings.CharacterBLASIntervalFrames;
    }
    if (observeCharacters)
        nativeCharacters.Advance(frame, actorLimit,
            observationInterval, playerOnly,
            stageCharacterGeometry, runCharacterSkinning && stageCharacterGeometry,
            validateCharacterSkinning && runCharacterSkinning && stageCharacterGeometry,
            buildIsolatedCharacterBLAS && validateCharacterSkinning &&
                runCharacterSkinning && stageCharacterGeometry,
            buildIsolatedCharacterTLAS && buildIsolatedCharacterBLAS &&
                validateCharacterSkinning && runCharacterSkinning &&
                stageCharacterGeometry,
            traceIsolatedCharacterRay && buildIsolatedCharacterTLAS &&
                buildIsolatedCharacterBLAS && validateCharacterSkinning &&
                runCharacterSkinning && stageCharacterGeometry,
            blasUpdateInterval, backend->d3d12Device.get(), backend->commandQueue.get());
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
	raster.Capture(*backend, inputs, Util::GetCurrentSceneDepthSRV(false), camera, frame, 0ull);
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
    const auto scope = ResolveNativeSpatialScope(globals::game::player, cell);
    if (!scope || scope->formID != cellID) return std::nullopt;
    if (settings.SubdivisionMode != 0 && settings.BuildCandidateGridTLAS &&
        settings.UseGridForNativeDXR && stableGridPublished &&
        stableGridPublished->cellID == cellID &&
        stableGridPublished->revision == revision &&
        stableGridPublished->cell && stableGridPublished->acceleration &&
        stableGridPublished->materials &&
        stableGridPublished->materials->scene == stableGridPublished->acceleration)
    {
        auto published = *stableGridPublished;
        published.bakedLights = globals::features::nativeLLF.GetSceneLightBake(published.cell);
        return published;  // Last complete scene even while recentering.
    }
	auto data = prepared.GetCell();
	auto scene = currentSceneStableAcceleration ? currentSceneStableAcceleration : acceleration.GetScene();
	if (!data || !scene || scene->scope.scene != revision) return std::nullopt;
	auto materials = currentSceneStableBindings ? currentSceneStableBindings : materialBindings.Get();
	if (materials && materials->scene != scene) materials.reset();
    auto baked = globals::features::nativeLLF.GetSceneLightBake(data);
	return PublishedScene{ std::move(data), std::move(scene), std::move(materials), cellID, revision, std::move(baked) };
}

void DX12NativeScene::PublishDiagnostics()
{
	const auto bindings = materialBindings.Get();
	const auto retainedPages = DX12Native::RetainedMaterialPageBytes();
	json snapshot{ { "enabled", settings.Enabled }, { "nativeOwnership", OwnsNativeFeatures() },
        { "globalDXGIHeadroomPercent", DX12Native::VideoMemoryBudget::HeadroomPercent() },
        { "subdivisionMode", settings.SubdivisionMode }, { "subcellSize", settings.SubcellSize },
        { "autoGridMemoryScreening", settings.AutoGridMemoryScreening },
        { "autoGridDecisionLatched", autoGridDecisionLatched },
        { "stableGridTLAS", bool(stableGridPublished) },
        { "gridHandoffs", gridHandoffs },
        { "stableGridCenter", { stableGridCenter.x, stableGridCenter.y } },
        { "sceneScope", [&]() {
            auto* player = globals::game::player;
            const auto scope = ResolveNativeSpatialScope(player,
                player ? player->GetParentCell() : nullptr);
            return scope ? scope->request.ScopedKey() : std::string("unavailable");
        }() },
        { "useGridForNativeDXR", settings.UseGridForNativeDXR },
        { "subcellGpuLoader", subcellManager.GetDiagnostics() },
        { "candidateGridTLAS", candidateGridAcceleration.GetDiagnostics() },
        { "candidateGridMaterialReady", [&]() {
            const auto scene = candidateGridAcceleration.GetScene();
            const auto bindings = candidateGridBindings.Get();
            return scene && bindings && bindings->scene == scene;
        }() },
        { "candidateGridMaterialFailure", candidateGridBindings.Failure() },
        { "candidateGridMaterialBudgetBytes", candidateMaterialAllowance },
        { "publishedGridToNativeDXR", [&]() {
            const auto selection = GetPublishedScene();
            return selection && selection->acceleration &&
                selection->acceleration->scope.policy == 3;
        }() },
		{ "nativePBR", globals::features::nativePBR.IsActive() }, { "nativeLLF", globals::features::nativeLLF.IsActive() },
		{ "nativeSCM", globals::features::nativeSCM.IsActive() },
        { "nativeCharacterObservation", nativeCharacters.GetDiagnostics() },
		{ "nativeLightSource", OwnsNativeFeatures() ? "prepared-priority-unified" : "prepared-cell" },
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
		{ "runtimeGeometry", { { "scmRigidCaptureEnabled", settings.CaptureSCMRigidCasters },
            { "scmNativeCollectionActive", runtimeGeometryCaptureActive.load(std::memory_order_acquire) &&
                globals::features::nativeSCM.IsActive() },
            { "capturedMeshes", runtimeGeometry.Count() },
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
            { "automatic", true },
            { "manualLimitBytes", uint64_t{0} },
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
            { "runtimeOnlyMaterialSources", globals::features::nativePBR.IsActive() && settings.ImportLoosePBR &&
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
