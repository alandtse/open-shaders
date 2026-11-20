#include "NativeFeatures.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "Features/NativePBR/MaterialRouting.h"
#include "Features/NativePBR/PBRHookDispatcher.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
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
#include "Features/NativePBR/RayMaterialBindings.h"
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

NativePBRFeature::NativePBRFeature() : materialWorker(std::make_shared<MaterialWorker>()) {}
NativePBRFeature::~NativePBRFeature() = default;

void NativePBRFeature::SetupResources()
{
	if (SelectedBackend() == NativePBR::Backend::NativeDX12)
		materialCatalog.Load();
}

void NativePBRFeature::DiscoverPreparedAssets()
{
	const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
	if (!scene || !scene->acceleration || discoveredGeometry == scene->acceleration)
		return;
	const auto catalog = materialCatalog.GetSnapshot();
	std::unordered_set<std::string> currentKeys;
	materialDiscovery.BeginInstanceEpoch();
	for (const auto& instance : scene->acceleration->instances) {
		const auto& identity = instance.preparedIdentity;
		const auto& material = instance.material;
		if (!material.opaqueCandidate || !material.offlineDescription ||
			identity.sourceDigest.empty() || identity.blockIndex == UINT32_MAX ||
			identity.referenceKey.empty() || identity.shapeName.empty())
			continue;
		const auto& source = *material.offlineDescription;
		if (!source.is_object() || !source.contains("shaderFlags1") || !source.contains("shaderFlags2") ||
			!source.contains("alpha") || !source.contains("textureClampMode") ||
			!source.contains("uvOffsetScale") || !source.at("uvOffsetScale").is_array() ||
			!source.contains("textures") || !source.at("textures").is_array())
			continue;
		const auto& uv = source.at("uvOffsetScale");
		const auto& textures = source.at("textures");
		if (uv.size() != 4 || textures.size() < 8)
			continue;
		try {
			const auto flags1 = source.at("shaderFlags1").get<uint32_t>();
			const auto flags2 = source.at("shaderFlags2").get<uint32_t>();
			const auto clamp = source.at("textureClampMode").get<uint32_t>();
			const auto alpha = source.at("alpha").get<float>();
			const std::array<float, 4> transform{ uv[0].get<float>(), uv[1].get<float>(), uv[2].get<float>(), uv[3].get<float>() };
			if (!std::isfinite(alpha) || alpha != 1.0f || clamp > 3 ||
				!std::ranges::all_of(transform, [](float value) { return std::isfinite(value); }))
				continue;
			NativePBR::MaterialSource materialSource;
			materialSource.base.materialAlpha = alpha;
			materialSource.base.clampMode = clamp;
			materialSource.base.uvOffsetScale = { transform, transform };
			const uint64_t flags = (uint64_t(flags2) << 32) | flags1;
			materialSource.base.legacyComplexMask =
				(flags & uint64_t(RE::BSShaderProperty::EShaderPropertyFlag::kEnvMap)) != 0;
			constexpr std::array<size_t, 7> nifSlots{ 0, 1, 5, 2, 3, 7, 6 };
			for (size_t role = 0; role < nifSlots.size(); ++role) {
				const auto& value = textures.at(nifSlots[role]);
				if (!value.is_string())
					continue;
				materialSource.base.texturePaths[role] = value.get<std::string>();
			}
			if ((flags2 & (1u << 23)) != 0) {
				auto has = [flags](RE::BSShaderProperty::EShaderPropertyFlag flag) {
					return (flags & uint64_t(flag)) != 0;
				};
				materialSource.nifFlags = PBRRecords::NifSurfaceFlags{
					.multiLayerParallax = has(RE::BSShaderProperty::EShaderPropertyFlag::kMultiLayerParallax),
					.backLighting = has(RE::BSShaderProperty::EShaderPropertyFlag::kBackLighting),
					.effectLighting = has(RE::BSShaderProperty::EShaderPropertyFlag::kEffectLighting),
					.anisotropicLighting = has(RE::BSShaderProperty::EShaderPropertyFlag::kAnisotropicLighting),
					.rimLighting = has(RE::BSShaderProperty::EShaderPropertyFlag::kRimLighting),
					.softLighting = has(RE::BSShaderProperty::EShaderPropertyFlag::kSoftLighting),
					.fitSlope = has(RE::BSShaderProperty::EShaderPropertyFlag::kFitSlope)
				};
			}
			const auto key = std::format("{}:{}", identity.sourceDigest, identity.blockIndex);
			if (currentKeys.insert(key).second) {
				const auto report = materialDiscovery.Register(key, std::move(materialSource), catalog);
				if (report.status == NativePBR::MaterialSourceReport::Status::InvalidParameters)
					continue;
			}
			NativePBR::InstanceProperties properties;
			properties.opacity = NativePBR::InstanceProperties::Opacity::Opaque;
			properties.normalMode = (flags1 & (1u << 12)) != 0 ?
				NativePBR::InstanceProperties::NormalMode::ModelSpace : NativePBR::InstanceProperties::NormalMode::TangentSpace;
			properties.uvAnimation = NativePBR::InstanceProperties::UvAnimation::Static;
			materialDiscovery.ObserveInstance(key, identity.referenceKey, identity.shapeName, std::move(properties));
		} catch (const std::exception&) {
			continue;
		}
	}
	for (const auto& key : discoveredMaterialKeys)
		if (!currentKeys.contains(key))
			materialDiscovery.Retire(key);
	discoveredMaterialKeys = std::move(currentKeys);
	discoveredGeometry = scene->acceleration;
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

std::shared_ptr<const NativePBR::RayMaterialBindings> NativePBRFeature::GetRayMaterialBindings() const
{
	return IsActive() ? rayMaterialBindings.load(std::memory_order_acquire) : nullptr;
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
		rayMaterialBindings.store({}, std::memory_order_release);
		return;
	}
	DiscoverPreparedAssets();
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
	const auto prepared = GetPreparedSceneMaterials();
	const auto materials = GetNativeGpuMaterials();
	const auto previous = rayMaterialBindings.load(std::memory_order_acquire);
	const bool forceAdmission = forceRayAdmission.load(std::memory_order_relaxed);
	const bool requireParallax = nativeParallax.load(std::memory_order_relaxed);
	if (!previous || previous->prepared != prepared || previous->materials != materials ||
		previous->forcedAdmission != forceAdmission || previous->parallaxRequired != requireParallax)
		rayMaterialBindings.store(NativePBR::PrepareRayMaterialBindings(prepared, materials, forceAdmission, requireParallax), std::memory_order_release);
}

void NativePBRFeature::OnSceneTransitionReset(bool)
{
	preparedSceneMaterials.store({}, std::memory_order_release);
	rayMaterialBindings.store({}, std::memory_order_release);
	discoveredGeometry.reset();
	materialDiscovery.BeginInstanceEpoch();
	for (const auto& key : discoveredMaterialKeys)
		materialDiscovery.Retire(key);
	discoveredMaterialKeys.clear();
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
	const auto nativeMask = nativeScene.GetShadowMask(frame);
	if (!prepared || !nativeMask || !nativeMask->readyFence || !nativeMask->readyValue ||
		nativeMask->readyValue == UINT64_MAX)
		return false;
	// NativeSCM selection must reference the prepared-first unified indices,
	// not the old, unrelated raw LLF array indices.
	const auto unified = globals::features::nativeLLF.BuildUnifiedLights(
		prepared->cell->nativeLights, frame, true);
	for (uint32_t index = 0; index < unified.count; ++index) {
		const auto& identity = unified.source[index];
		if (!identity.valid ||
			(identity.flags & static_cast<uint32_t>(PointLightFlags::Flags::Shadow)) == 0 ||
			identity.shadowSlot != static_cast<uint32_t>(slot))
			continue;

		const bool maskSelected = nativeScene.settings.ShadowMask &&
		                          (nativeScene.settings.ShadowAllLights || nativeScene.settings.ShadowLightIndex == index);
		if (!maskSelected)
			continue;
		const auto maskIndex = nativeScene.settings.ShadowAllLights ? index : 0u;
		if (maskIndex >= nativeMask->lightSnapshot.size())
			continue;
		const auto& traced = nativeMask->lightSnapshot[maskIndex];
		const auto& preparedLight = unified.lights[index];
		const float mx = traced.position.x - preparedLight.position.x;
		const float my = traced.position.y - preparedLight.position.y;
		const float mz = traced.position.z - preparedLight.position.z;
		if (!std::isfinite(mx) || !std::isfinite(my) || !std::isfinite(mz) ||
			mx * mx + my * my + mz * mz > 1.0f)
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
		bool active, float units, float gain, bool contribute, bool contributeMatches);
}

void NativeLLFFeature::ClearCapturedLights()
{
	{
		std::scoped_lock lock(lightMutex);
		lightFrame.reset();
	}
	// Drop stale light generations without waiting for workers.
	workerUnified.store({}, std::memory_order_release);
	fallbackUnified.store({}, std::memory_order_release);
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
		work->matchUnits = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
		work->gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
		work->contribute = contributeUnmatched.load(std::memory_order_relaxed) && work->gain > 0.0f;
		work->contributeMatches = contributeMatched.load(std::memory_order_relaxed) && work->gain > 0.0f;
		static BS::thread_pool<> pool{ 1 };
		pool.detach_task([work] {
			try {
				work->result = ComputeNativeUnifiedLights(
					work->prepared->nativeLights, work->source.get(), true,
					work->matchUnits, work->gain, work->contribute, work->contributeMatches);
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
		bool active, float units, float gain, bool contribute, bool contributeMatches)
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
				if (closest < result.prepared) {
					++result.matched;
					if (contributeMatches) {
						auto& radiance = result.lights[closest].radiance;
						const float3 combined{ radiance.x + live.radiance.x * gain,
							radiance.y + live.radiance.y * gain, radiance.z + live.radiance.z * gain };
						if (std::isfinite(combined.x) && std::isfinite(combined.y) && std::isfinite(combined.z)) {
							radiance = combined;
							++result.matchedContributed;
						}
					}
				} else
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
	const bool contributeMatches = contributeMatched.load(std::memory_order_relaxed) && gain > 0.0f;
	const auto matches = [&](const std::shared_ptr<const UnifiedWork>& work) {
		return work && work->done.load(std::memory_order_acquire) &&
		       work->valid && work->source == published && work->prepared &&
		       work->prepared->nativeLights.data() == prepared.data() &&
		       work->prepared->nativeLights.size() == prepared.size() &&
		       work->matchUnits == units && work->gain == gain &&
		       work->contribute == contribute && work->contributeMatches == contributeMatches;
	};
	if (active && published) {
		if (auto work = workerUnified.load(std::memory_order_acquire); matches(work)) {
			workerHits.fetch_add(1, std::memory_order_relaxed);
			return work->result;
		}
		if (auto cached = fallbackUnified.load(std::memory_order_acquire); matches(cached))
			return cached->result;
	}
	auto result = ComputeNativeUnifiedLights(prepared, published.get(), active, units, gain, contribute, contributeMatches);
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
			cached->contributeMatches = contributeMatches;
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
	bool compatibility = backendSettings.LegacyCompatibilityMaterials;
	if (ImGui::Checkbox("Legacy Compatibility Materials (DX11 fallback; restart required)", &compatibility))
		backendSettings.LegacyCompatibilityMaterials = compatibility;
	if (bootSnapshot.HasPendingChange(backendSettings, &BackendSettings::LegacyCompatibilityMaterials))
		Util::Text::RestartNeeded("%s", "Legacy material shader change pending restart");
	ImGui::TextDisabled("Opt-in: preserves Extended Materials parallax and complex shading on raster characters and other DX11 fallback objects.");
	ImGui::TextDisabled("TruePBR remains disabled in native mode; DX12 prepared materials are evaluated independently.");
	DrawOwnership("Enable Native PBR", enabled, IsActive());
	bool parallax = nativeParallax.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Native DX12 parallax (experimental)", &parallax))
		nativeParallax.store(parallax, std::memory_order_relaxed);
	bool complex = nativeComplexMaterials.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Native DX12 complex materials (experimental)", &complex))
		nativeComplexMaterials.store(complex, std::memory_order_relaxed);
	ImGui::TextDisabled("Both are opt-in and affect only native ray-hit shading; basic PBR remains active.");
	ImGui::TextDisabled("Legacy Compatibility Materials is a separate DX11 raster setting.");
	ImGui::SeparatorText("Native Procedural Snow");
	bool coverEligible = snowCoverEligible.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Cover eligible snow receivers (interior test)", &coverEligible))
		snowCoverEligible.store(coverEligible, std::memory_order_relaxed);
	ImGui::TextDisabled("Session-only: tests interior receivers; terrain coverage is the default.");
	int stage = static_cast<int>(readinessStage.load(std::memory_order_relaxed)) - 1;
	if (ImGui::Combo("DX12 ownership readiness (testing)", &stage,
		"Geometry confirmed (incomplete)\0NativePBR evaluated (incomplete)\0Native lighting completed (safe)\0"))
		readinessStage.store(static_cast<uint32_t>(stage + 1), std::memory_order_relaxed);
	if (stage < 2)
		ImGui::TextDisabled("Diagnostic: native-first pixels may be black or missing light; DX11 fallback remains for other pixels.");
	bool forceAdmission = forceRayAdmission.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Force opaque ray-material admission (testing)", &forceAdmission))
		forceRayAdmission.store(forceAdmission, std::memory_order_relaxed);
	if (forceAdmission)
		ImGui::TextDisabled("Unsafe test: ignores recipe/instance readiness and missing DDS roles; requires a real matched recipe and safe geometry.");
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
	ImGui::TextDisabled("NativePBR material sources come from companion NIF metadata and native JSON catalogs.");
	const auto materialView = GetMaterialPublication();
	const auto gpuMaterials = GetNativeGpuMaterials();
	const auto preparedMaterials = GetPreparedSceneMaterials();
	const auto rayBindings = GetRayMaterialBindings();
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
	ImGui::Text("NativePBR ray bindings: %u eligible | %u unsupported | %u unavailable%s",
		rayBindings ? rayBindings->eligible : 0, rayBindings ? rayBindings->unsupported : 0,
		rayBindings ? rayBindings->unavailable : 0, rayBindings ? "" : " (not published)");
	if (materialWorker && !materialWorker->gpu.Failure().empty())
		ImGui::TextDisabled("Native material GPU preparation: %s", materialWorker->gpu.Failure().c_str());
}
void NativeLLFFeature::DrawSettings()
{
	DrawOwnership("Enable Native LLF", enabled, IsActive());
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
	const auto lights = globals::features::nativeLLF.GetLightFrame(frame);
	ImGui::Text("Captured current-frame LLF lights: %zu", lights ? lights->lights.size() : size_t{ 0 });
	bool add = contributeUnmatched.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Contribute unmatched LLF lights", &add))
		contributeUnmatched.store(add, std::memory_order_relaxed);
	bool matched = contributeMatched.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Add matched LLF radiance (testing)", &matched))
		contributeMatched.store(matched, std::memory_order_relaxed);
	float gain = contribution.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("NativeLLF contribution", &gain, 0.0f, 4.0f, "%.2fx"))
		contribution.store(gain, std::memory_order_relaxed);
	ImGui::TextDisabled("Scales opted-in LLF radiance; matched test adds to prepared color, not position or radius.");
	float units = matchDistanceUnits.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("Clustering distance (Skyrim units)", &units, 0.0f, 512.0f, "%.0f units"))
		matchDistanceUnits.store(units, std::memory_order_relaxed);
	ImGui::TextDisabled("Prepared lights remain first and preserve their original light properties.");
	ImGui::TextDisabled("Matched test can double-count an emitter already represented by the companion.");
	const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
	if (scene && globals::game::graphicsState) {
		const auto merged = BuildUnifiedLights(scene->cell->nativeLights, frame);
		ImGui::Text("Prepared %u | matched LLF %u (%u contributing) | added LLF %u | unified %u/%u",
			merged.prepared, merged.matched, merged.matchedContributed, merged.added, merged.count, kMaxUnifiedLights);
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
	backendSettings.LegacyCompatibilityMaterials = j.value("LegacyCompatibilityMaterials", false);
	enabled.store(j.value("Enabled", true), std::memory_order_relaxed);
	nativeParallax.store(j.value("NativeParallax", false), std::memory_order_relaxed);
	nativeComplexMaterials.store(j.value("NativeComplexMaterials", false), std::memory_order_relaxed);
	snowCoverEligible.store(false, std::memory_order_relaxed);
	forceRayAdmission.store(false, std::memory_order_relaxed);
	readinessStage.store(std::clamp(j.value("NativeReadinessStage", 3u), 1u, 3u), std::memory_order_relaxed);
}
void NativeLLFFeature::LoadSettings(json& j)
{
	enabled.store(j.value("Enabled", true), std::memory_order_relaxed);
	contributeUnmatched.store(j.value("ContributeUnmatched", true), std::memory_order_relaxed);
	contributeMatched.store(j.value("ContributeMatched", false), std::memory_order_relaxed);
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
		{ "LegacyCompatibilityMaterials", backendSettings.LegacyCompatibilityMaterials },
		{ "NativeParallax", nativeParallax.load(std::memory_order_relaxed) },
		{ "NativeComplexMaterials", nativeComplexMaterials.load(std::memory_order_relaxed) },
		{ "NativeReadinessStage", readinessStage.load(std::memory_order_relaxed) } };
}
void NativePBRFeature::RegisterUxActions()
{
	FEATURE_COMMAND("configureSnow", "Toggle the session-only interior snow receiver test: coverEligible (bool).", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativePBRFeature*>(self);
		if (args.contains("coverEligible"))
			feature.snowCoverEligible.store(args.at("coverEligible").get<bool>(), std::memory_order_relaxed);
	});
}
void NativeLLFFeature::SaveSettings(json& j)
{
	j = { { "Enabled", enabled.load(std::memory_order_relaxed) },
		{ "ContributeUnmatched", contributeUnmatched.load(std::memory_order_relaxed) },
		{ "ContributeMatched", contributeMatched.load(std::memory_order_relaxed) },
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
	const auto rayBindings = GetRayMaterialBindings();
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
		{ "nativeParallax", nativeParallax.load(std::memory_order_relaxed) },
		{ "nativeComplexMaterials", nativeComplexMaterials.load(std::memory_order_relaxed) },
		{ "nativeFirstReadinessStage", NativeFirstReadinessStage() },
		{ "legacyCompatibilityMaterials", SelectedBackend() == NativePBR::Backend::NativeDX12 &&
			bootSnapshot.Boot(&BackendSettings::LegacyCompatibilityMaterials) },
		{ "nativeJsonTextureSets", catalog ? catalog->textureSets.size() : 0 },
		{ "nativeJsonMaterialObjects", catalog ? catalog->materialObjects.size() : 0 },
		{ "nativeJsonRejectedTextureSets", catalog ? catalog->rejectedTextureSets : 0 },
		{ "nativeJsonRejectedMaterialObjects", catalog ? catalog->rejectedMaterialObjects : 0 },
		{ "backendRestartPending", bootSnapshot.HasPendingChange(backendSettings, &BackendSettings::NativeBackend) },
		{ "selectedBackend", state.backend == NativePBR::Backend::NativeDX12 ? "dx12-native" : "dx11-legacy" },
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
		{ "rayMaterialBindings", { { "published", bool(rayBindings) },
			{ "forcedAdmission", forceRayAdmission.load(std::memory_order_relaxed) },
			{ "eligible", rayBindings ? rayBindings->eligible : 0 },
			{ "unsupported", rayBindings ? rayBindings->unsupported : 0 },
			{ "unavailable", rayBindings ? rayBindings->unavailable : 0 } } },
		{ "gpuMaterialFailure", materialWorker ? materialWorker->gpu.Failure() : std::string{} },
		{ "materialInputsAvailable", state.materialInputsAvailable },
		{ "publishedNativeMaterialGeneration", state.publishedNativeMaterialGeneration },
		{ "renderingOwnerPolicy", state.owner == NativePBR::RenderingOwner::Native ? "native" :
			(state.owner == NativePBR::RenderingOwner::NativeUnavailable ? "native-unavailable" : "legacy") },
		{ "legacyComparisonRequested", state.legacyComparisonRequested } };
}
json NativeLLFFeature::GetDiagnostics()
{
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
	const auto lights = globals::features::nativeLLF.GetLightFrame(frame);
	json report{ { "selected", enabled.load() }, { "native", IsActive() },
		{ "contributeUnmatched", contributeUnmatched.load() },
		{ "contributeMatched", contributeMatched.load() },
		{ "matchDistanceUnits", matchDistanceUnits.load() },
		{ "contribution", contribution.load() },
		{ "observedLights", lights ? lights->lights.size() : size_t{ 0 } },
		{ "workerQueued", workerQueued.load(std::memory_order_relaxed) },
		{ "workerResultsConsumed", workerHits.load(std::memory_order_relaxed) },
		{ "synchronousFallbackCalls", fallbackHits.load(std::memory_order_relaxed) } };
	const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
	if (scene && globals::game::graphicsState) {
		const auto merged = BuildUnifiedLights(scene->cell->nativeLights, frame);
		report["preparedLights"] = merged.prepared;
		report["unifiedLights"] = merged.count;
		report["matchedLLFLights"] = merged.matched;
		report["matchedLLFContributions"] = merged.matchedContributed;
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
