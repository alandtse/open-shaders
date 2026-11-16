#include "NativeFeatures.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "Features/NativePBR/MaterialRouting.h"
#include "Features/NativePBR/PBRHookDispatcher.h"
#include "Utils/D3D.h"
#include "Utils/UI.h"
#include "Features/NativeLighting/NativeLighting.h"
#include "Features/NativeComposite/NativeComposite.h"
#include "Globals.h"
#include "State.h"
#include "ShaderCache.h"
#include "Features/NativePBR/NativePBRCompiler.h"
#include "Features/NativePBR/MaterialService.h"
#include "Features/NativePBR/GpuMaterialResources.h"
#include "Features/NativePBR/PreparedSceneMaterials.h"
#include "Features/NativePBR/SurfaceFramePublisher.h"
#include "Utils/PointLightFlags.h"
#include <BS_thread_pool.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

struct NativePBRFeature::MaterialWorker
{
	NativePBR::RecipeCache cache;
	NativePBR::MaterialService service;
	NativePBR::GpuMaterialWorker gpu;
	std::mutex mutex;
	MaterialSummary summary;
	std::atomic<bool> active{ false }, more{ false };
	std::atomic<uint64_t> revision{ 0 };
	std::atomic<std::shared_ptr<const NativePBR::RecipeGeneration>> published;
};

NativePBRFeature::NativePBRFeature() : materialWorker(std::make_shared<MaterialWorker>()),
	surfacePublisher(std::make_unique<NativePBR::SurfaceFramePublisher>()) {}
NativePBRFeature::~NativePBRFeature() = default;

void NativePBRFeature::SetupResources()
{
	if (SelectedBackend() == NativePBR::Backend::NativeDX12)
		materialCatalog.Load();
}

std::shared_ptr<const NativePBR::RecipeGeneration> NativePBRFeature::GetNativeRecipes() const
{
	if (!materialWorker)
		return {};
	const auto generation = materialWorker->published.load(std::memory_order_acquire);
	return generation && generation->revision == materialRegistry.Revision() ? generation : nullptr;
}

std::shared_ptr<const NativePBR::MaterialPublication> NativePBRFeature::GetMaterialPublication() const
{
	return materialWorker ? materialWorker->service.GetSnapshotForRegistry(materialRegistry) : nullptr;
}

std::shared_ptr<const NativePBR::GpuMaterialResources> NativePBRFeature::GetNativeGpuMaterials() const
{
	return materialWorker ? materialWorker->gpu.Get() : nullptr;
}

std::shared_ptr<const NativePBR::PreparedSceneMaterials> NativePBRFeature::GetPreparedSceneMaterials() const
{
	return IsActive() ? preparedSceneMaterials.load(std::memory_order_acquire) : nullptr;
}

NativePBRFeature::MaterialSummary NativePBRFeature::GetMaterialSummary() const
{
	MaterialSummary result;
	if (materialWorker) {
		std::scoped_lock lock(materialWorker->mutex);
		result = materialWorker->summary;
	}
	return result;
}

void NativePBRFeature::PollMaterialRegistry()
{
	const auto worker = materialWorker;
	if (worker->active.load(std::memory_order_acquire) ||
		(!worker->more.load(std::memory_order_relaxed) &&
			worker->revision.load(std::memory_order_relaxed) == materialRegistry.Revision()))
		return;
	worker->active.store(true, std::memory_order_release);
	try {
		static BS::thread_pool<> pool{ 1 };
		pool.detach_task([worker, registry = &materialRegistry, catalog = &materialCatalog] {
			try {
				if (auto changes = registry->ExportChanges(worker->cache.Revision(), worker->cache.KnownRevisions())) {
					worker->cache.Update(*changes);
					const auto generation = NativePBR::PublishRecipes(worker->cache, *changes,
						worker->published.load(std::memory_order_relaxed));
					if (!worker->service.Publish(generation, catalog->GetSnapshot(), changes->instanceHints, changes->instanceEpoch))
						throw std::runtime_error("NativePBR material publication rejected a stale generation");
					worker->published.store(generation, std::memory_order_release);
					worker->more.store(changes->hasMore, std::memory_order_relaxed);
				}
				MaterialSummary summary;
				summary.recipes = worker->cache.Recipes().size();
				summary.pending = worker->cache.Pending();
				for (const auto& [identity, recipe] : worker->cache.Recipes()) {
					if (recipe.issues & ~NativePBR::UnknownOpacity)
						++summary.incomplete;
					for (const auto& texture : recipe.textures) {
						if (!texture.resource.texture2D)
							continue;
						++summary.observedTextures;
						if (texture.resource.miscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
							++summary.sharedCandidates;
					}
					for (size_t issue = 0; issue < summary.issues.size(); ++issue)
						if (recipe.issues & (1u << issue))
							++summary.issues[issue];
				}
				{
					std::scoped_lock lock(worker->mutex);
					worker->summary = summary;
				}
				worker->revision.store(worker->cache.Revision(), std::memory_order_relaxed);
			} catch (...) {
				{
					std::scoped_lock lock(worker->mutex);
					worker->summary.failed = true;
				}
				worker->revision.store(registry->Revision(), std::memory_order_relaxed);
				worker->more.store(false, std::memory_order_relaxed);
			}
			worker->active.store(false, std::memory_order_release);
		});
	} catch (...) {
		worker->active.store(false, std::memory_order_release);
		{
			std::scoped_lock lock(worker->mutex);
			worker->summary.failed = true;
		}
		worker->revision.store(materialRegistry.Revision(), std::memory_order_relaxed);
		worker->more.store(false, std::memory_order_relaxed);
	}
}

void NativePBRFeature::Prepass()
{
	if (SelectedBackend() != NativePBR::Backend::NativeDX12 || !enabled.load(std::memory_order_relaxed)) {
		preparedSceneMaterials.store({}, std::memory_order_release);
		return;
	}
	PollMaterialRegistry();
	const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
	const auto materialPublication = GetMaterialPublication();
	if (scene && scene->acceleration && scene->materials && materialPublication) {
		const auto previous = preparedSceneMaterials.load(std::memory_order_acquire);
		if (!previous || previous->geometry != scene->acceleration ||
			previous->sceneResources != scene->materials || previous->materialPublication != materialPublication ||
			previous->cellID != scene->cellID || previous->sceneRevision != scene->revision)
			preparedSceneMaterials.store(NativePBR::PrepareSceneMaterials(scene->acceleration,
				scene->materials, materialPublication, scene->cellID, scene->revision), std::memory_order_release);
	} else {
		preparedSceneMaterials.store({}, std::memory_order_release);
	}
	if (auto backend = globals::features::dx12NativeScene.GetGraphicsBackend())
		if (auto recipes = GetNativeRecipes(); recipes && recipes->batchesComplete)
			if (const auto prepared = GetPreparedSceneMaterials(); prepared && prepared->sceneResources &&
				prepared->sceneResources->materials)
				materialWorker->gpu.Request(*backend, std::move(recipes), 0, 16ull * 1024 * 1024,
					prepared->sceneResources->materials);
	materialWorker->gpu.Advance();
}

void NativePBRFeature::OnSceneTransitionReset(bool)
{
	preparedSceneMaterials.store({}, std::memory_order_release);
}

// Backend selection is the ONLY automatic execution switch. Feature toggles
// are explicit user choices, never material/caster/light validity gates.
void NativePBRFeature::LatchBackendAtBoot()
{
	std::call_once(backendLatchOnce, [this] {
		bootSnapshot.Latch(backendSettings);
		bootBackend.store(NativePBR::SelectBackend(backendSettings.NativeBackend,
			globals::features::dx12NativeScene.IsNativeBackendRequested(), globals::game::isVR), std::memory_order_relaxed);
		backendLatched.store(true, std::memory_order_release);
	});
}

NativePBR::Backend NativePBRFeature::SelectedBackend() const
{
	if (backendLatched.load(std::memory_order_acquire))
		return bootBackend.load(std::memory_order_relaxed);
	return NativePBR::SelectBackend(backendSettings.NativeBackend,
		globals::features::dx12NativeScene.IsNativeBackendRequested(), globals::game::isVR);
}

void NativePBRFeature::PostPostLoad()
{
	LatchBackendAtBoot();
	if (SelectedBackend() == NativePBR::Backend::NativeDX12)
		PBRHooks::Install();
}

bool NativePBRFeature::IsActive() const
{
	return NativePBR::ResolveRouting({
		.backend = SelectedBackend(),
		.nativeFeatureEnabled = enabled.load(std::memory_order_relaxed),
		.featureLoaded = loaded,
		.sceneOwnsNative = globals::features::dx12NativeScene.OwnsNativeFeatures(),
		.scope = NativePBR::MaterialScope::OpaqueStaticMesh
	}).owner == NativePBR::RenderingOwner::Native;
}

bool NativePBRFeature::OwnsInputOnlyShading() const
{
	return !globals::game::isVR && IsActive() && experimentalInputOnly.load(std::memory_order_relaxed) &&
		globals::features::nativeComposite.IsEnabled();
}

void NativePBRFeature::PrepareSidecar(ID3D11Device* a_device)
{
	if (!globals::game::isVR)
		materialSidecar.Prepare(a_device);
}

void NativePBRFeature::BeginOpaqueSidecar()
{
	if (!IsActive() || globals::game::isVR || !globals::game::renderer || !globals::game::graphicsState)
		return;
	// The expensive full-resolution MRT sidecar only exists while an explicit
	// consumer needs it. Material recipes and DX12 DDS residency are independent.
	// Keep the preview and projected-input experiments available on demand.
	const bool captureRequested = globals::features::nativeComposite.WantsNativePbrSurfaceDiagnostic() ||
		OwnsInputOnlyShading() || previewOpaqueReplacement.load(std::memory_order_relaxed) ||
		captureProjectedUv.load(std::memory_order_relaxed);
	if (!captureRequested) {
		surfaceHandoffStatus = "NativePBR surface capture idle (no active consumer)";
		return;
	}
	surfaceHandoffStatus = "Awaiting opaque raster completion";
	const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
	auto* texture = Util::AsReal(target.texture);
	if (!texture)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	materialSidecar.BeginFrame(globals::d3d::device, globals::d3d::context, Util::AsReal(target.RTV),
		globals::game::graphicsState->GetFrameCount(), desc.Width, desc.Height,
		captureProjectedUv.load(std::memory_order_relaxed), OwnsInputOnlyShading());
}

void NativePBRFeature::BeginSidecarPass(uint64_t a_identity, uint64_t a_revision, std::string a_referenceKey,
	std::string a_shapeName, NativePBR::PreviewSurveyClass a_surveyClass, uint32_t a_unsupportedReasons)
{
	materialSidecar.BeginPass(a_identity, a_revision, std::move(a_referenceKey), std::move(a_shapeName),
		a_surveyClass, a_unsupportedReasons);
}
void NativePBRFeature::EndSidecarPass() { materialSidecar.EndPass(); }
bool NativePBRFeature::TryInputOnlyPass(const RE::BSShader& a_shader, uint32_t a_descriptor)
{
	if (!materialSidecar.InputOnly() || !IsActive() || globals::game::isVR)
		return false;
	const bool nativeDxilAvailable = std::filesystem::exists("Data/Shaders/NativeComposite/NativePBRInput.dxil");
	auto* shader = nativeDxilAvailable && globals::shaderCache ?
		globals::shaderCache->GetNativePBRInputPixelShader(a_shader, a_descriptor) : nullptr;
	return materialSidecar.StageInputOnlyShader(shader);
}
bool NativePBRFeature::BeginInputOnlyDraw(ID3D11DeviceContext* a_context)
{
	return IsActive() && !globals::game::isVR && materialSidecar.BeginInputOnlyPass(a_context);
}
void NativePBRFeature::EndInputOnlyDraw() { materialSidecar.EndInputOnlyDraw(); }
void NativePBRFeature::ReplaySidecarDraw(ID3D11DeviceContext* a_context, UINT a_count, UINT a_first, INT a_base, bool a_inputBound)
{
	if (IsActive() && !globals::game::isVR)
	{
		const auto mode = previewOpaqueReplacement.load(std::memory_order_relaxed) ?
			static_cast<NativePBR::PreviewMode>(1u + std::min(previewVisualization.load(std::memory_order_relaxed), 2u)) :
			NativePBR::PreviewMode::Off;
		materialSidecar.ReplayDrawIndexed(a_context, a_count, a_first, a_base, mode, a_inputBound);
	}
}
void NativePBRFeature::FinishOpaqueSidecar(ID3D11ShaderResourceView* a_finalDepth)
{
	materialSidecar.FinishFrame(globals::d3d::context, a_finalDepth);
}
void NativePBRFeature::BindSurfaceInterop(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
	ID3D12Device* a_device12, ID3D12CommandQueue* a_queue12)
{
	if (!a_device11 || !a_context11 || !a_device12 || !a_queue12) {
		surfacePublisher.reset();
		surfaceDevice11 = nullptr;
		surfaceContext11 = nullptr;
		surfaceDevice12 = nullptr;
		surfaceQueue12 = nullptr;
		return;
	}
	if (!surfacePublisher || surfaceDevice11.get() != a_device11 || surfaceContext11.get() != a_context11 ||
		surfaceDevice12.get() != a_device12 || surfaceQueue12.get() != a_queue12)
		surfacePublisher = std::make_unique<NativePBR::SurfaceFramePublisher>();
	surfaceDevice11.copy_from(a_device11);
	surfaceContext11.copy_from(a_context11);
	surfaceDevice12.copy_from(a_device12);
	surfaceQueue12.copy_from(a_queue12);
}

void NativePBRFeature::OnOpaqueRasterReady(const OpaqueRasterInputs&)
{
	++surfaceHandoffCalls;
	if (!IsActive() || globals::game::isVR || !globals::game::graphicsState) {
		if (surfacePublisher)
			surfacePublisher->ReleaseIdle();
		surfaceHandoffStatus = "Native material capture inactive for this runtime";
		return;
	}
	if (!globals::features::nativeComposite.WantsNativePbrSurfaceDiagnostic()) {
		if (surfacePublisher)
			surfacePublisher->ReleaseIdle();
		surfaceHandoffStatus = "Native Composite material diagnostic disabled";
		return;
	}
	if (!surfacePublisher || !surfaceDevice11 || !surfaceContext11 || !surfaceDevice12 || !surfaceQueue12) {
		surfaceHandoffStatus = "DX12 surface publisher or interop device not initialized";
		return;
	}
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const auto attachments = materialSidecar.GetSources(frame);
	const auto draws = materialSidecar.GetDrawTable();
	if (!attachments[0] || !draws || draws->empty()) {
		surfaceHandoffStatus = std::format("Sidecar not ready: requested frame={}, recorded frame={}, draw count={}, status={}",
			frame, materialSidecar.GetFrame(), materialSidecar.PublishedDrawCount(), materialSidecar.Status());
		return;
	}
	NativePBR::SurfaceFramePublisher::Sources sources;
	sources.textures = attachments;
	sources.draws = draws;
	sources.frame = frame;
	sources.nativeOwnedDraws = true;
	const bool published = surfacePublisher->Publish(surfaceDevice11.get(), surfaceContext11.get(),
		surfaceDevice12.get(), surfaceQueue12.get(), sources);
	surfaceHandoffStatus = published ? "Independent ABI-v3 surface published" : surfacePublisher->GetDiagnostics().status;
}

std::shared_ptr<const NativePBR::SurfaceFrame> NativePBRFeature::GetSurfaceFrame(uint32_t a_frame) const
{
	return IsActive() && surfacePublisher ? surfacePublisher->Get(a_frame) : nullptr;
}

NativePBR::MaterialSidecar::Sources NativePBRFeature::GetSidecarSources(uint32_t a_frame) const
{
	return IsActive() ? materialSidecar.GetSources(a_frame) : NativePBR::MaterialSidecar::Sources{};
}
bool NativeLLFFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }
bool NativeSCMFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }

bool NativeSCMFeature::OwnsStaticForLight(const RE::BSShadowLight* source, int32_t slot) const
{
	if (!source || slot < 0 || !IsActive() || !globals::features::nativeLLF.IsActive() ||
		!globals::game::graphicsState)
		return false;

	// SCM shadow rendering is scheduled before LLF publishes the *current*
	// frame's raster light buffer. Use the last complete LLF generation, and
	// match BOTH slot and world-space light identity: a reassigned slot must
	// never inherit another light's native shadow ownership.
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const auto published = globals::features::nativeLLF.GetRecentLightFrame(frame);
	const auto* niLight = source->light.get();
	if (!published || !niLight)
		return false;

	const auto& nativeScene = globals::features::dx12NativeScene;
	const auto prepared = nativeScene.GetPublishedScene();
	if (!prepared)
		return false;
	// NativeSCM selection must reference the prepared-first unified indices,
	// not the old, unrelated raw LLF array indices.
	const auto unified = globals::features::nativeLLF.BuildUnifiedLights(
		prepared->cell->nativeLights, frame, true);
	const auto& direct = globals::features::nativeLighting;
	for (uint32_t index = 0; index < unified.count; ++index) {
		const auto& identity = unified.source[index];
		if (!identity.valid ||
			(identity.flags & static_cast<uint32_t>(PointLightFlags::Flags::Shadow)) == 0 ||
			identity.shadowSlot != static_cast<uint32_t>(slot))
			continue;

		const bool maskSelected = nativeScene.settings.ShadowMask &&
		                          (nativeScene.settings.ShadowAllLights || nativeScene.settings.ShadowLightIndex == index);
		const bool directSelected = direct.SelectsLightForNativeShadows(index);
		if (!maskSelected && !directSelected)
			continue;

		const auto& p = niLight->world.translate;
		const float dx = p.x - identity.position.x, dy = p.y - identity.position.y, dz = p.z - identity.position.z;
		const float tolerance = std::clamp(identity.radius * 0.01f, 4.0f, 32.0f);
		if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
			dx * dx + dy * dy + dz * dz <= tolerance * tolerance)
			return true;
	}
	return false;
}

namespace
{
	NativeLLFFeature::UnifiedLights ComputeNativeUnifiedLights(
		std::span<const DX12Native::PreparedPointLight> prepared,
		const NativeLLFFeature::LightFrame* published,
		bool active, float units, float gain, bool contribute);
	std::shared_ptr<const DX12Native::NativeSceneLightBake> BuildNativeSceneLightBake(
		const NativeLLFFeature::UnifiedLights& unified, uint32_t frame,
		const std::shared_ptr<const DX12Native::PreparedCell>& baseline,
		const std::shared_ptr<const DX12Native::NativeSceneLightBake>& previous,
		float matchUnits, float gain);
}

void NativeLLFFeature::ClearCapturedLights()
{
	{
		std::scoped_lock lock(lightMutex);
		lightFrame.reset();
	}
	// Drop all stale scene/frame generations without waiting for workers.
	workerUnified.store({}, std::memory_order_release);
	fallbackUnified.store({}, std::memory_order_release);
	bakeEpoch.fetch_add(1, std::memory_order_acq_rel);
	sceneBake.store({}, std::memory_order_release);
}

void NativeLLFFeature::OnSceneTransitionReset(bool a_open)
{
	transitionOpen.store(a_open, std::memory_order_release);
	ClearCapturedLights();
}

void NativeLLFFeature::PublishLights(const LightLimitFix::LightData* data, uint32_t count, const float3& eye, uint32_t frame)
{
	if (!IsActive() || transitionOpen.load(std::memory_order_acquire)) {
		ClearCapturedLights();
		return;
	}
	auto generation = std::make_shared<LightFrame>();
	generation->frame = frame;
	generation->lights.reserve(count);
	for (uint32_t i = 0; data && i < count; ++i) {
		const auto& source = data[i];
		if (source.lightFlags.any(LightLimitFix::LightFlags::Disabled))
			continue;
		const auto& p = source.positionWS[0].data;
		const Light light{
			{ p.x + eye.x, p.y + eye.y, p.z + eye.z },
			{ source.color.x * source.fade, source.color.y * source.fade, source.color.z * source.fade },
			source.radius, std::sqrt(std::max(source.sizeBias, 0.0f)), source.fadeZone,
			static_cast<uint32_t>(source.lightFlags.underlying()), source.shadowMapIndex
		};
		if (!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z) ||
			!std::isfinite(light.radiance.x) || !std::isfinite(light.radiance.y) || !std::isfinite(light.radiance.z) ||
			!std::isfinite(light.radius) || light.radius <= 0.0f)
			continue;
		generation->lights.push_back(light);
	}
	{
		std::scoped_lock lock(lightMutex);
		// A loading transition may have started while the new snapshot was built.
		// Never republish the old cell after its clear operation.
		if (transitionOpen.load(std::memory_order_acquire)) {
			lightFrame.reset();
			return;
		}
		lightFrame = generation;
	}
	// NativeLLF CPU work only: schedule prepared-first matching after owning
	// the live snapshot. On late completion, readers use the exact synchronous
	// fallback rather than a stale/partial native light generation.
	const auto pending = workerUnified.load(std::memory_order_acquire);
	if (pending && !pending->done.load(std::memory_order_acquire))
		return;
	const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
	if (!scene || !scene->cell || !IsActive())
		return;
	try {
		auto work = std::make_shared<UnifiedWork>();
		work->source = generation;
		work->prepared = scene->cell;
		work->previousBake = scene->bakedLights;
		work->bakeEpoch = bakeEpoch.load(std::memory_order_acquire);
		work->matchUnits = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
		work->gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
		work->contribute = contributeUnmatched.load(std::memory_order_relaxed) && work->gain > 0.0f;
		static BS::thread_pool<> pool{ 1 };
		pool.detach_task([work] {
			try {
				work->result = ComputeNativeUnifiedLights(
					work->prepared->nativeLights, work->source.get(), true,
					work->matchUnits, work->gain, work->contribute);
				work->baked = BuildNativeSceneLightBake(
					work->result, work->source->frame, work->prepared,
					work->previousBake, work->matchUnits, work->gain);
				work->valid = true;
			} catch (...) { /* Never consume incomplete output. */
			}
			work->done.store(true, std::memory_order_release);
		});
		workerUnified.store(std::move(work), std::memory_order_release);
		workerQueued.fetch_add(1, std::memory_order_relaxed);
	} catch (...) { /* Preserve the exact legacy-compatible native fallback. */
	}
}
std::shared_ptr<const DX12Native::NativeSceneLightBake> NativeLLFFeature::GetSceneLightBake(
	const std::shared_ptr<const DX12Native::PreparedCell>& baseline) const
{
	if (!baseline || !IsActive() ||
		!bakeUnmatchedIntoScene.load(std::memory_order_relaxed) ||
		!contributeUnmatched.load(std::memory_order_relaxed))
		return {};
	const auto gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
	const auto units = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
	if (gain <= 0.0f)
		return {};
	// Publish only the worker's fully completed generation, exactly scoped to
	// this baseline and reset epoch. No engine pointers or GPU waits involved.
	const auto work = workerUnified.load(std::memory_order_acquire);
	if (work && work->done.load(std::memory_order_acquire) && work->valid &&
		work->baked && work->prepared == baseline &&
		work->bakeEpoch == bakeEpoch.load(std::memory_order_acquire)) {
		auto current = sceneBake.load(std::memory_order_acquire);
		if ((!current || current->generationFrame < work->baked->generationFrame) &&
			sceneBake.compare_exchange_strong(current, work->baked,
				std::memory_order_acq_rel, std::memory_order_acquire))
			bakedPublications.fetch_add(1, std::memory_order_relaxed);
	}
	auto baked = sceneBake.load(std::memory_order_acquire);
	return baked && baked->baseline == baseline &&
	               baked->matchUnits == units && baked->contributionGain == gain ?
	           baked :
	           nullptr;
}

std::shared_ptr<const NativeLLFFeature::LightFrame> NativeLLFFeature::GetLightFrame(uint32_t frame) const
{
	std::scoped_lock lock(lightMutex);
	return !transitionOpen.load(std::memory_order_acquire) && lightFrame && lightFrame->frame == frame ? lightFrame : nullptr;
}
std::shared_ptr<const NativeLLFFeature::LightFrame> NativeLLFFeature::GetRecentLightFrame(uint32_t frame) const
{
	std::scoped_lock lock(lightMutex);
	return !transitionOpen.load(std::memory_order_acquire) && lightFrame && uint32_t(frame - lightFrame->frame) <= 1u ? lightFrame : nullptr;
}

namespace
{
	NativeLLFFeature::UnifiedLights ComputeNativeUnifiedLights(
		std::span<const DX12Native::PreparedPointLight> prepared,
		const NativeLLFFeature::LightFrame* published,
		bool active, float units, float gain, bool contribute)
	{
		NativeLLFFeature::UnifiedLights result{};
		result.prepared = static_cast<uint32_t>(prepared.size());
		result.count = static_cast<uint32_t>(std::min(prepared.size(), result.lights.size()));
		std::copy_n(prepared.begin(), result.count, result.lights.begin());
		result.overflow = result.prepared - result.count;
		// No live LLF data must NEVER erase the authoritative prepared lighting.
		if (!active || !published)
			return result;
		result.observed = static_cast<uint32_t>(published->lights.size());
		const float radiusSquared = units * units;
		std::array<float, NativeLLFFeature::kMaxUnifiedLights> identityDistances{};
		identityDistances.fill(std::numeric_limits<float>::infinity());
		const auto distanceSquared = [](const float3& a, const float3& b) {
			const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
			return x * x + y * y + z * z;
		};
		for (const auto& live : published->lights) {
			uint32_t closest = UINT32_MAX;
			float bestSquared = std::numeric_limits<float>::infinity();
			// First match all supported prepared sources, regardless of whether
			// LLF augmentation is enabled. Unsupported prepared placeholders must
			// never suppress valid live illumination.
			for (uint32_t i = 0; i < std::min(result.prepared, result.count); ++i) {
				if (!result.lights[i].supported)
					continue;
				const float dist2 = distanceSquared(live.position, result.lights[i].position);
				if (dist2 <= radiusSquared && dist2 < bestSquared) {
					closest = i;
					bestSquared = dist2;
				}
			}
			// Only if no prepared match exists, deduplicate additional LLF sources.
			for (uint32_t i = std::min(result.prepared, result.count); closest == UINT32_MAX && i < result.count; ++i) {
				const float dist2 = distanceSquared(live.position, result.lights[i].position);
				if (dist2 <= radiusSquared && dist2 < bestSquared) {
					closest = i;
					bestSquared = dist2;
				}
			}
			if (closest != UINT32_MAX) {
				if (closest < result.prepared)
					++result.matched;
				else
					++result.clusteredLive;
				const auto& previous = result.source[closest];
				const bool shadow = (live.flags & static_cast<uint32_t>(PointLightFlags::Flags::Shadow)) != 0;
				const bool priorShadow = previous.valid &&
				                         (previous.flags & static_cast<uint32_t>(PointLightFlags::Flags::Shadow)) != 0;
				if (!previous.valid || (shadow && !priorShadow) ||
					(shadow == priorShadow && bestSquared < identityDistances[closest])) {
					result.source[closest] = { live.position, live.radius, live.flags, live.shadowSlot, true };
					identityDistances[closest] = bestSquared;
				}
				continue;
			}
			if (!contribute) {
				++result.omitted;
				continue;
			}
			if (result.count == result.lights.size()) {
				++result.overflow;
				continue;
			}
			const uint32_t target = result.count++;
			result.lights[target] = { live.position,
				{ live.radiance.x * gain, live.radiance.y * gain, live.radiance.z * gain },
				live.radius, live.sourceRadius, true };
			result.source[target] = { live.position, live.radius, live.flags, live.shadowSlot, true };
			identityDistances[target] = 0.0f;
			++result.added;
		}
		return result;
	}
	// Scene-scope lighting bake: stable, unprepared emitters accumulate in the
	// native worker's immutable snapshot. This is not disk persistence: LLF input
	// carries no stable placed-form identity with which to validate a disk entry.
	std::shared_ptr<const DX12Native::NativeSceneLightBake> BuildNativeSceneLightBake(
		const NativeLLFFeature::UnifiedLights& unified, uint32_t frame,
		const std::shared_ptr<const DX12Native::PreparedCell>& baseline,
		const std::shared_ptr<const DX12Native::NativeSceneLightBake>& previous,
		float matchUnits, float gain)
	{
		auto result = std::make_shared<DX12Native::NativeSceneLightBake>();
		result->baseline = baseline;
		result->generationFrame = frame;
		result->matchUnits = matchUnits;
		result->contributionGain = gain;
		constexpr uint32_t kLifetimeFrames = 30;
		const auto distanceSquared = [](const float3& a, const float3& b) {
			const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
			return x * x + y * y + z * z;
		};
		const auto matchesPrepared = [&](const DX12Native::PreparedPointLight& light) {
			for (uint32_t i = 0; i < std::min(unified.prepared, unified.count); ++i) {
				if (unified.lights[i].supported &&
					distanceSquared(light.position, unified.lights[i].position) <= matchUnits * matchUnits)
					return true;
			}
			return false;
		};
		if (previous && previous->baseline == baseline &&
			previous->matchUnits == matchUnits && previous->contributionGain == gain) {
			for (const auto& entry : previous->candidates) {
				if (frame >= entry.lastSeenFrame &&
					frame - entry.lastSeenFrame <= kLifetimeFrames &&
					!matchesPrepared(entry.light))
					result->candidates.push_back(entry);
			}
		}
		std::vector<bool> touched(result->candidates.size(), false);
		constexpr uint32_t rejectedFlags =
			static_cast<uint32_t>(PointLightFlags::Flags::Particle) |
			static_cast<uint32_t>(PointLightFlags::Flags::Spot) |
			static_cast<uint32_t>(PointLightFlags::Flags::PortalStrict);
		for (uint32_t i = std::min(unified.prepared, unified.count); i < unified.count; ++i) {
			const auto& light = unified.lights[i];
			const auto& source = unified.source[i];
			if (!source.valid || !light.supported ||
				(source.flags & rejectedFlags) != 0 || matchesPrepared(light))
				continue;
			size_t match = result->candidates.size();
			float best = std::numeric_limits<float>::infinity();
			for (size_t j = 0; j < result->candidates.size(); ++j) {
				if (touched[j])
					continue;
				const auto& prior = result->candidates[j].light;
				const float tolerance = std::clamp(prior.radius * 0.005f, 1.0f, 8.0f);
				const float d = distanceSquared(light.position, prior.position);
				if (d <= tolerance * tolerance && d < best &&
					std::abs(light.radius - prior.radius) <= std::max(4.0f, prior.radius * 0.03f)) {
					match = j;
					best = d;
				}
			}
			if (match < result->candidates.size()) {
				auto& entry = result->candidates[match];
				touched[match] = true;
				entry.consecutiveObservations = frame > entry.lastSeenFrame &&
				                                        frame - entry.lastSeenFrame <= 4 ?
				                                    std::min(entry.consecutiveObservations + 1u, 1024u) :
				                                    1u;
				entry.light = light;
				entry.lastSeenFrame = frame;
			} else if (result->candidates.size() < NativeLLFFeature::kMaxUnifiedLights) {
				result->candidates.push_back({ light, frame, 1u });
				touched.push_back(true);
			}
		}
		return result;
	}
}  // namespace

NativeLLFFeature::UnifiedLights NativeLLFFeature::BuildUnifiedLights(
	std::span<const DX12Native::PreparedPointLight> prepared, uint32_t frame, bool allowRecent) const
{
	const bool active = IsActive();
	const auto published = active ?
	                           (allowRecent ? GetRecentLightFrame(frame) : GetLightFrame(frame)) :
	                           nullptr;
	const float units = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
	const float gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
	const bool contribute = contributeUnmatched.load(std::memory_order_relaxed) && gain > 0.0f;
	const auto matches = [&](const std::shared_ptr<const UnifiedWork>& work) {
		return work && work->done.load(std::memory_order_acquire) &&
		       work->valid && work->source == published && work->prepared &&
		       work->prepared->nativeLights.data() == prepared.data() &&
		       work->prepared->nativeLights.size() == prepared.size() &&
		       work->matchUnits == units && work->gain == gain &&
		       work->contribute == contribute;
	};
	if (active && published) {
		if (auto work = workerUnified.load(std::memory_order_acquire); matches(work)) {
			workerHits.fetch_add(1, std::memory_order_relaxed);
			return work->result;
		}
		if (auto cached = fallbackUnified.load(std::memory_order_acquire); matches(cached))
			return cached->result;
	}
	auto result = ComputeNativeUnifiedLights(prepared, published.get(), active, units, gain, contribute);
	if (active && published) {
		auto cached = std::make_shared<UnifiedWork>();
		cached->source = std::move(published);
		// Only the authoritative native scene owns the source span. A
		// matching shared owner is needed for safe future cache reads.
		if (const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
			scene && scene->cell && scene->cell->nativeLights.data() == prepared.data() &&
			scene->cell->nativeLights.size() == prepared.size()) {
			cached->prepared = scene->cell;
			cached->matchUnits = units;
			cached->gain = gain;
			cached->contribute = contribute;
			cached->result = result;
			cached->valid = true;
			cached->done.store(true, std::memory_order_release);
			fallbackUnified.store(std::move(cached), std::memory_order_release);
		}
		fallbackHits.fetch_add(1, std::memory_order_relaxed);
	}
	return result;
}

namespace
{
	void DrawOwnership(const char* title, std::atomic<bool>& enabled, bool active)
	{
		bool selected = enabled.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(title, &selected))
			enabled.store(selected, std::memory_order_relaxed);
		ImGui::TextDisabled("Rendering owner: %s", active ? "Native DX12" : "Legacy / inactive");
		ImGui::TextDisabled("Uses Skyrim's existing hooks; never installs a competing engine detour.");
	}
}

void NativePBRFeature::DrawSettings()
{
	bool nativeBackend = backendSettings.NativeBackend;
	if (ImGui::Checkbox("Use DX12-Native PBR backend (restart required)", &nativeBackend))
		backendSettings.NativeBackend = nativeBackend;
	ImGui::TextDisabled("Boot PBR backend: %s", SelectedBackend() == NativePBR::Backend::NativeDX12 ? "DX12-Native" : "Legacy DX11");
	if (bootSnapshot.HasPendingChange(backendSettings, &BackendSettings::NativeBackend))
		Util::Text::RestartNeeded("%s", "PBR backend change pending restart");
	DrawOwnership("Enable Native PBR", enabled, IsActive());
	if (const auto catalog = materialCatalog.GetSnapshot())
		ImGui::Text("Native JSON sources: %zu texture sets / %zu MATO | rejected: %zu / %zu",
			catalog->textureSets.size(), catalog->materialObjects.size(),
			catalog->rejectedTextureSets, catalog->rejectedMaterialObjects);
	if (ImGui::TreeNode("Native recipe issues")) {
		const auto summary = GetMaterialSummary();
		ImGui::Text("Recipes: %zu | pending: %zu | incomplete: %zu", summary.recipes, summary.pending, summary.incomplete);
		if (summary.failed)
			ImGui::TextDisabled("Recipe compilation failed; inspect the native material log.");
		for (size_t index = 0; index < summary.issues.size(); ++index) {
			if (summary.issues[index] == 0)
				continue;
			const auto name = NativePBR::kRecipeIssueNames[index];
			ImGui::BulletText("%.*s: %zu", static_cast<int>(name.size()), name.data(), summary.issues[index]);
		}
		ImGui::TreePop();
	}
	ImGui::TextDisabled("NativePBR owns live material recipes; current engine observations still originate in TruePBR hooks.");
	const auto materialView = GetMaterialPublication();
	const auto gpuMaterials = GetNativeGpuMaterials();
	const auto preparedMaterials = GetPreparedSceneMaterials();
	ImGui::Text("Material publication: %llu | instance bindings: %zu",
		materialView ? materialView->serial : 0, materialView ? materialView->instances.size() : 0);
	ImGui::Text("Prepared scene handoff: %zu instances | matched: %u | missing: %u | ambiguous: %u",
		preparedMaterials ? preparedMaterials->instances.size() : 0,
		preparedMaterials ? preparedMaterials->available : 0,
		preparedMaterials ? preparedMaterials->missing : 0,
		preparedMaterials ? preparedMaterials->ambiguous : 0);
	ImGui::Text("DX12 GPU materials: %zu | fully resident: %u | partial: %u | limited: %u",
		gpuMaterials ? gpuMaterials->entries.size() : 0, gpuMaterials ? gpuMaterials->fullyResident : 0,
		gpuMaterials ? gpuMaterials->partiallyResident : 0, gpuMaterials ? gpuMaterials->featureLimited : 0);
	if (materialWorker && !materialWorker->gpu.Failure().empty())
		ImGui::TextDisabled("Native material GPU preparation: %s", materialWorker->gpu.Failure().c_str());
	ImGui::Text("%s: %llu | rejected: %llu", materialSidecar.InputOnly() ? "Input-only raster draws" : "Sidecar raster draws",
		materialSidecar.ReplayedDraws(), materialSidecar.RejectedDraws());
	ImGui::Text("Last frame's PBR surfaces: %zu | frame: %u", materialSidecar.PublishedDrawCount(), materialSidecar.GetFrame());
	if (surfacePublisher) {
		const auto& publication = surfacePublisher->GetDiagnostics();
		ImGui::Text("DX12 surface publications: %llu | busy slots: %llu | budget deferrals: %llu",
			publication.published, publication.busySlots, publication.budgetDeferrals);
		ImGui::TextDisabled("Surface publisher: %s", publication.status.c_str());
	}
	ImGui::TextDisabled("Surface handoff (%llu opaque hooks): %s", surfaceHandoffCalls, surfaceHandoffStatus.c_str());
	if (materialSidecar.InputOnly())
		ImGui::Text("FXC input draws this frame: %u | shader unavailable: %u | RT rejected: %u",
			materialSidecar.InputOnlyDrawsThisFrame(), materialSidecar.InputShaderMissesThisFrame(),
			materialSidecar.InputTargetRejectionsThisFrame());
	if (materialSidecar.InputOnly())
		ImGui::Text("Geometry passes: %u | PBR candidates: %u | missing identity: %u | draw limit: %u",
			materialSidecar.SetupPassesThisFrame(), materialSidecar.PbrCandidatesThisFrame(),
			materialSidecar.MissingPbrIdentitiesThisFrame(), materialSidecar.InputCapacityMissesThisFrame());
	if (materialSidecar.InputOnly()) {
		const auto reasons = materialSidecar.UnsupportedReasonsThisFrame();
		ImGui::Text("Unsupported observations: %u | projected: %u | model-space: %u | landscape: %u | other: %u",
			materialSidecar.UnsupportedDrawsThisFrame(), reasons[0], reasons[1], reasons[2], reasons[3]);
	}
	bool projected = captureProjectedUv.load(std::memory_order_relaxed);
	bool inputOnly = experimentalInputOnly.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Capture input-only FXC draws (keep raster shading)", &inputOnly))
		experimentalInputOnly.store(inputOnly, std::memory_order_relaxed);
	if (inputOnly)
		ImGui::TextDisabled("FXC sidecar captures inputs even when optional input-only shader variants are unavailable; raster shading stays active.");
	if (ImGui::Checkbox("Capture ProjectedUV material inputs (experimental)", &projected))
		captureProjectedUv.store(projected, std::memory_order_relaxed);
	if (projected) {
		ImGui::Text("ProjectedUV %s draws this frame: %u/%u", inputOnly ? "input-only" : "sidecar",
			materialSidecar.ProjectedDrawsThisFrame(), materialSidecar.SurveyLimit());
		if (!inputOnly)
			ImGui::TextDisabled("Input capture only; projected shading remains on legacy TruePBR.");
	}
	bool preview = previewOpaqueReplacement.load(std::memory_order_relaxed);
	ImGui::BeginDisabled(inputOnly);
	if (ImGui::Checkbox("Preview opaque PBR replacement (temporary)", &preview))
		previewOpaqueReplacement.store(preview, std::memory_order_relaxed);
	ImGui::EndDisabled();
	if (preview) {
		int mode = static_cast<int>(std::min(previewVisualization.load(std::memory_order_relaxed), 2u));
		if (ImGui::Combo("Preview visualization", &mode, "Material identity\0UV / derivatives\0Coverage comparison\0"))
			previewVisualization.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
		switch (mode) {
		case 1:
			ImGui::TextDisabled("R/G: wrapped UV; B: UV derivative footprint. White: UV grid.");
			break;
		case 2:
			ImGui::TextDisabled("Green: registered PBR; amber: PBR candidate without identity.");
			ImGui::TextDisabled("Blue: ordinary static. Cyan: projected UV. Yellow: model-space normals.");
			ImGui::TextDisabled("Magenta: landscape. Purple: other PBR exclusions.");
			ImGui::TextDisabled("Survey-only geometry is never admitted to native shading.");
			break;
		default:
			ImGui::TextDisabled("Stable per-material colors and UV grid.");
			break;
		}
	}
	if (preview && !materialSidecar.PreviewReady())
		ImGui::TextDisabled("Preview shader unavailable; check FXC compile log.");
	ImGui::Text("Preview draws: %llu", materialSidecar.PreviewDraws());
	if (preview && previewVisualization.load(std::memory_order_relaxed) == 2u)
	{
		ImGui::Text("Unregistered PBR candidate draws: %llu", materialSidecar.PreviewMissingIdentities());
		ImGui::Text("Ordinary static preview draws: %llu | unsupported PBR: %llu",
			materialSidecar.PreviewOrdinaryDraws(), materialSidecar.PreviewUnsupportedDraws());
		const auto reasons = materialSidecar.UnsupportedReasonsThisFrame();
		ImGui::Text("This frame: projected UV %u | model-space normals %u", reasons[0], reasons[1]);
		ImGui::Text("Landscape %u | other %u (overlaps counted)", reasons[2], reasons[3]);
		ImGui::Text("Survey-only draws this frame: %u/%u", materialSidecar.SurveyDrawsThisFrame(), materialSidecar.SurveyLimit());
	}
	ImGui::TextDisabled("Depth-equal raster preview only; no GPU readback or native PBR lighting.");
	if (!materialSidecar.Status().empty())
		ImGui::TextWrapped("Sidecar: %s", materialSidecar.Status().c_str());
	ImGui::TextDisabled(inputOnly ? "Capture-only until SM6.5 material replacement is ready; final color remains raster." :
		"Commit 3 captures surface identity only; native shading is not enabled yet.");
}
void NativeLLFFeature::DrawSettings()
{
	DrawOwnership("Enable Native LLF", enabled, IsActive());
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
	const auto lights = globals::features::nativeLLF.GetLightFrame(frame);
	ImGui::Text("Captured current-frame LLF lights: %zu", lights ? lights->lights.size() : size_t{ 0 });
	bool sceneBakeEnabled = bakeUnmatchedIntoScene.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Bake stable LLF-only lights into native scene", &sceneBakeEnabled))
		bakeUnmatchedIntoScene.store(sceneBakeEnabled, std::memory_order_relaxed);
	ImGui::TextDisabled("Runtime scene layer; four stable observations and 30-frame expiry.");
	ImGui::TextDisabled("No disk persistence without a verified placed-light identity.");
	bool add = contributeUnmatched.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Contribute unmatched LLF lights", &add))
		contributeUnmatched.store(add, std::memory_order_relaxed);
	float gain = contribution.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("NativeLLF contribution", &gain, 0.0f, 4.0f, "%.2fx"))
		contribution.store(gain, std::memory_order_relaxed);
	ImGui::TextDisabled("Scales unmatched LLF emitter radiance only; prepared lighting remains unchanged.");
	float units = matchDistanceUnits.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("Clustering distance (Skyrim units)", &units, 0.0f, 512.0f, "%.0f units"))
		matchDistanceUnits.store(units, std::memory_order_relaxed);
	ImGui::TextDisabled("Prepared lights remain first and preserve their original light properties.");
	ImGui::TextDisabled("Nearby LLF lights are folded into their prepared matches, never added twice.");
	const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
	if (scene && globals::game::graphicsState) {
		const auto merged = BuildUnifiedLights(scene->cell->nativeLights, frame);
		ImGui::Text("Prepared %u | matched LLF %u | added LLF %u | unified %u/%u",
			merged.prepared, merged.matched, merged.added, merged.count, kMaxUnifiedLights);
		ImGui::Text("LLF clustered %u | omitted %u | overflow %u",
			merged.clusteredLive, merged.omitted, merged.overflow);
	}
	ImGui::TextDisabled("LLF-only lights participate in DXR shadows against prepared TLAS geometry.");
	ImGui::TextDisabled("Moving objects require their geometry in the TLAS for fully RT-cast shadows.");
	ImGui::TextDisabled("Up to 64 unified lights per native dispatch (prepared-first, then LLF).");
}
void NativeSCMFeature::DrawSettings()
{
	DrawOwnership("Enable Native SCM", enabled, IsActive());
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
	const auto casters = ShadowCasterManager::GetNativeCasterFrame(frame);
	ImGui::Text("Tracked current-frame casters: %zu", casters ? casters->casters.size() : size_t{ 0 });
	ImGui::Text("Native TLAS-covered static: %u | awaiting rigid transfer: %u",
		casters ? casters->preparedStaticRetired : 0u,
		casters ? casters->uncoveredRigid : 0u);
	ImGui::Text("Unconverted movers: %u | skinned: %u | fallback raster: %u",
		casters ? casters->moving : 0u,
		casters ? casters->skinned : 0u,
		casters ? casters->rasterFallback : 0u);
	ImGui::TextDisabled("Prepared static casters belong to native TLAS; remaining casters use SCM tracking.");
	ImGui::TextDisabled("Matched LLF lights: raster dynamic caster depth only; prepared geometry: native DXR.");
	ImGui::TextDisabled("Uncovered rigid CPU-backed casters may enter the native DX12 geometry mailbox.");
	ImGui::TextDisabled("DX12 owns BLAS/TLAS and RayQuery; moving and skinned casters still need updates.");
}
void NativePBRFeature::LoadSettings(json& j)
{
	backendSettings.NativeBackend = j.value("NativeBackend", true);
	enabled.store(j.value("Enabled", true), std::memory_order_relaxed);
	captureProjectedUv.store(j.value("CaptureProjectedUv", false), std::memory_order_relaxed);
	experimentalInputOnly.store(j.value("ExperimentalInputOnly", false), std::memory_order_relaxed);
	previewOpaqueReplacement.store(j.value("PreviewOpaqueReplacement", false), std::memory_order_relaxed);
	previewVisualization.store(static_cast<uint32_t>(std::clamp(j.value("PreviewVisualization", 0), 0, 2)),
		std::memory_order_relaxed);
}
void NativeLLFFeature::LoadSettings(json& j)
{
	enabled.store(j.value("Enabled", true), std::memory_order_relaxed);
	contributeUnmatched.store(j.value("ContributeUnmatched", true), std::memory_order_relaxed);
	bakeUnmatchedIntoScene.store(j.value("BakeUnmatchedIntoScene", true), std::memory_order_relaxed);
	const float value = j.value("MatchDistanceUnits", 64.0f);
	matchDistanceUnits.store(std::isfinite(value) ? std::clamp(value, 0.0f, 512.0f) : 64.0f,
		std::memory_order_relaxed);
	const float strength = j.value("Contribution", 1.0f);
	contribution.store(std::isfinite(strength) ? std::clamp(strength, 0.0f, 4.0f) : 1.0f,
		std::memory_order_relaxed);
}
void NativeSCMFeature::LoadSettings(json& j) { enabled.store(j.value("Enabled", true), std::memory_order_relaxed); }
void NativePBRFeature::SaveSettings(json& j)
{
	j = { { "Enabled", enabled.load(std::memory_order_relaxed) },
		{ "NativeBackend", backendSettings.NativeBackend },
		{ "CaptureProjectedUv", captureProjectedUv.load(std::memory_order_relaxed) },
		{ "ExperimentalInputOnly", experimentalInputOnly.load(std::memory_order_relaxed) },
		{ "PreviewOpaqueReplacement", previewOpaqueReplacement.load(std::memory_order_relaxed) },
		{ "PreviewVisualization", previewVisualization.load(std::memory_order_relaxed) } };
}
void NativeLLFFeature::SaveSettings(json& j)
{
	j = { { "Enabled", enabled.load(std::memory_order_relaxed) },
		{ "ContributeUnmatched", contributeUnmatched.load(std::memory_order_relaxed) },
		{ "BakeUnmatchedIntoScene", bakeUnmatchedIntoScene.load(std::memory_order_relaxed) },
		{ "MatchDistanceUnits", matchDistanceUnits.load(std::memory_order_relaxed) },
		{ "Contribution", contribution.load(std::memory_order_relaxed) } };
}
void NativeSCMFeature::SaveSettings(json& j) { j = { { "Enabled", enabled.load(std::memory_order_relaxed) } }; }
json NativePBRFeature::GetDiagnostics()
{
	const auto recipes = GetNativeRecipes();
	const auto materialView = GetMaterialPublication();
	const auto gpuMaterials = GetNativeGpuMaterials();
	const auto preparedMaterials = GetPreparedSceneMaterials();
	const auto catalog = materialCatalog.GetSnapshot();
	const auto summary = GetMaterialSummary();
	json issueCounts = json::object();
	for (size_t index = 0; index < summary.issues.size(); ++index)
		if (summary.issues[index] != 0)
			issueCounts[std::string(NativePBR::kRecipeIssueNames[index])] = summary.issues[index];
	const auto state = NativePBR::ResolveRouting({
		.backend = SelectedBackend(),
		.nativeFeatureEnabled = enabled.load(std::memory_order_relaxed),
		.featureLoaded = loaded,
		.sceneOwnsNative = globals::features::dx12NativeScene.OwnsNativeFeatures(),
		.materialInputsAvailable = recipes && !recipes->recipes.empty(),
		.scope = NativePBR::MaterialScope::OpaqueStaticMesh
	});
	return { { "selected", enabled.load() }, { "native", state.owner == NativePBR::RenderingOwner::Native },
		{ "nativeJsonTextureSets", catalog ? catalog->textureSets.size() : 0 },
		{ "nativeJsonMaterialObjects", catalog ? catalog->materialObjects.size() : 0 },
		{ "nativeJsonRejectedTextureSets", catalog ? catalog->rejectedTextureSets : 0 },
		{ "nativeJsonRejectedMaterialObjects", catalog ? catalog->rejectedMaterialObjects : 0 },
		{ "backendRestartPending", bootSnapshot.HasPendingChange(backendSettings, &BackendSettings::NativeBackend) },
		{ "selectedBackend", state.backend == NativePBR::Backend::NativeDX12 ? "dx12-native" : "dx11-legacy" },
		{ "experimentalInputOnly", experimentalInputOnly.load(std::memory_order_relaxed) },
		{ "exclusiveInputOnlyShading", OwnsInputOnlyShading() },
		{ "captureProjectedUv", captureProjectedUv.load(std::memory_order_relaxed) },
		{ "projectedDrawsThisFrame", materialSidecar.ProjectedDrawsThisFrame() },
		{ "inputOnlyAdmittedDraws", materialSidecar.InputOnlyDrawsThisFrame() },
		{ "inputOnlyShaderMisses", materialSidecar.InputShaderMissesThisFrame() },
		{ "inputOnlyTargetRejections", materialSidecar.InputTargetRejectionsThisFrame() },
		{ "inputOnlyDrawLimitMisses", materialSidecar.InputCapacityMissesThisFrame() },
		{ "unsupportedObservedDraws", materialSidecar.UnsupportedDrawsThisFrame() },
		{ "setupPassesThisFrame", materialSidecar.SetupPassesThisFrame() },
		{ "pbrCandidatesThisFrame", materialSidecar.PbrCandidatesThisFrame() },
		{ "missingPbrIdentitiesThisFrame", materialSidecar.MissingPbrIdentitiesThisFrame() },
		{ "recipeRevision", materialRegistry.Revision() },
		{ "recipeCount", summary.recipes }, { "recipePending", summary.pending },
		{ "recipeIncomplete", summary.incomplete }, { "recipeCompileFailed", summary.failed },
		{ "recipeIssueCounts", std::move(issueCounts) },
		{ "materialPublicationSerial", materialView ? materialView->serial : 0 },
		{ "materialInstanceBindings", materialView ? materialView->instances.size() : 0 },
		{ "preparedSceneHandoff", { { "version", NativePBR::kPreparedSceneMaterialsVersion },
			{ "sceneRevision", preparedMaterials ? preparedMaterials->sceneRevision : 0 },
			{ "materialSerial", preparedMaterials ? preparedMaterials->materialSerial : 0 },
			{ "instances", preparedMaterials ? preparedMaterials->instances.size() : 0 },
			{ "matched", preparedMaterials ? preparedMaterials->available : 0 },
			{ "missing", preparedMaterials ? preparedMaterials->missing : 0 },
			{ "ambiguous", preparedMaterials ? preparedMaterials->ambiguous : 0 } } },
		{ "gpuMaterialCount", gpuMaterials ? gpuMaterials->entries.size() : 0 },
		{ "gpuFullyResident", gpuMaterials ? gpuMaterials->fullyResident : 0 },
		{ "gpuPartiallyResident", gpuMaterials ? gpuMaterials->partiallyResident : 0 },
		{ "gpuFeatureLimited", gpuMaterials ? gpuMaterials->featureLimited : 0 },
		{ "surfaceHandoffStatus", surfaceHandoffStatus },
		{ "surfaceHandoffCalls", surfaceHandoffCalls },
		{ "gpuMaterialFailure", materialWorker ? materialWorker->gpu.Failure() : std::string{} },
		{ "surfacePublication", surfacePublisher ? json{ { "published", surfacePublisher->GetDiagnostics().published },
			{ "busySlots", surfacePublisher->GetDiagnostics().busySlots },
			{ "budgetDeferrals", surfacePublisher->GetDiagnostics().budgetDeferrals },
			{ "invalidDrawTables", surfacePublisher->GetDiagnostics().invalidDrawTables },
			{ "unavailableSources", surfacePublisher->GetDiagnostics().unavailableSources },
			{ "status", surfacePublisher->GetDiagnostics().status } } : json::object() },
		{ "previewOpaqueReplacement", previewOpaqueReplacement.load(std::memory_order_relaxed) },
		{ "previewVisualization", previewVisualization.load(std::memory_order_relaxed) },
		{ "previewDraws", materialSidecar.PreviewDraws() },
		{ "previewMissingIdentities", materialSidecar.PreviewMissingIdentities() },
		{ "previewOrdinaryDraws", materialSidecar.PreviewOrdinaryDraws() },
		{ "previewUnsupportedDraws", materialSidecar.PreviewUnsupportedDraws() },
		{ "previewUnsupportedReasons", materialSidecar.PreviewUnsupportedReasons() },
		{ "unsupportedReasonsThisFrame", materialSidecar.UnsupportedReasonsThisFrame() },
		{ "surveyDrawsThisFrame", materialSidecar.SurveyDrawsThisFrame() },
		{ "shaderDomainPolicy", state.shaderDomain == NativePBR::ShaderDomain::NativePBRInput ? "native-pbr-input" :
			(state.shaderDomain == NativePBR::ShaderDomain::Unavailable ? "unmigrated" : "legacy-true-pbr") },
		{ "materialInputsAvailable", state.materialInputsAvailable },
		{ "publishedNativeMaterialGeneration", state.publishedNativeMaterialGeneration },
		{ "renderingOwnerPolicy", state.owner == NativePBR::RenderingOwner::Native ? "native" :
			(state.owner == NativePBR::RenderingOwner::NativeUnavailable ? "native-unavailable" : "legacy") },
		{ "legacyComparisonRequested", state.legacyComparisonRequested },
		{ "materialSidecar", { { "frame", materialSidecar.GetFrame() }, { "surfaces", materialSidecar.PublishedDrawCount() },
			{ "replayedDraws", materialSidecar.ReplayedDraws() }, { "rejectedDraws", materialSidecar.RejectedDraws() },
			{ "failedFrames", materialSidecar.FailedFrames() }, { "status", materialSidecar.Status() } } } };
}
json NativeLLFFeature::GetDiagnostics()
{
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
	const auto lights = globals::features::nativeLLF.GetLightFrame(frame);
	json report{ { "selected", enabled.load() }, { "native", IsActive() },
		{ "contributeUnmatched", contributeUnmatched.load() },
		{ "matchDistanceUnits", matchDistanceUnits.load() },
		{ "contribution", contribution.load() },
		{ "observedLights", lights ? lights->lights.size() : size_t{ 0 } },
		{ "sceneBakeEnabled", bakeUnmatchedIntoScene.load(std::memory_order_relaxed) },
		{ "sceneBakePublications", bakedPublications.load(std::memory_order_relaxed) },
		{ "sceneBakeCandidates", sceneBake.load(std::memory_order_acquire) ?
									 sceneBake.load(std::memory_order_acquire)->candidates.size() :
									 size_t{ 0 } },
		{ "workerQueued", workerQueued.load(std::memory_order_relaxed) },
		{ "workerResultsConsumed", workerHits.load(std::memory_order_relaxed) },
		{ "synchronousFallbackCalls", fallbackHits.load(std::memory_order_relaxed) } };
	const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
	if (scene && globals::game::graphicsState) {
		const auto merged = BuildUnifiedLights(scene->cell->nativeLights, frame);
		report["preparedLights"] = merged.prepared;
		report["unifiedLights"] = merged.count;
		report["matchedLLFLights"] = merged.matched;
		report["addedLLFLights"] = merged.added;
		report["rtShadowEligibleLLFLights"] = merged.added;
		report["clusteredLLFLights"] = merged.clusteredLive;
		report["omittedLLFLights"] = merged.omitted;
		report["overflowLights"] = merged.overflow;
	}
	return report;
}
json NativeSCMFeature::GetDiagnostics()
{
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
	const auto casters = ShadowCasterManager::GetNativeCasterFrame(frame);
	return { { "selected", enabled.load() }, { "native", IsActive() },
		{ "observedCasters", casters ? casters->casters.size() : size_t{ 0 } },
		{ "retiredPreparedStatic", casters ? casters->preparedStaticRetired : 0u },
		{ "uncoveredRigid", casters ? casters->uncoveredRigid : 0u },
		{ "movingNotConverted", casters ? casters->moving : 0u },
		{ "skinnedNotConverted", casters ? casters->skinned : 0u },
		{ "rasterFallback", casters ? casters->rasterFallback : 0u },
		{ "evaluationOwner", "DX12 RayQuery only for actually published TLAS casters" } };
}
