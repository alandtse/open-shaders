#include "Features/DX12NativeScene/ArchiveDdsSource.h"
#include "Features/DX12NativeScene/LiveGpuTextureEligibility.h"
#include "Features/DX12NativeScene/LiveLooseTexturePlan.h"
#include "Features/DX12NativeScene/LogicalTextureRegistry.h"
#include "Features/DX12NativeScene/MaterialDiskCache.h"
#include "Features/DX12NativeScene/MaterialManifestPolicy.h"
#include "Features/DX12NativeScene/MaterialPageCache.h"
#include "Features/DX12NativeScene/MaterialTextureLookup.h"
#include "Features/DX12NativeScene/MaterialUploadStatus.h"
#include "Features/DX12NativeScene/RuntimeGeometryCapture.h"
#include "Features/DX12NativeScene/RuntimeGeometryDiskFormat.h"
#include "Features/DX12NativeScene/RuntimeGeometryEncoding.h"
#include "Features/DX12NativeScene/RuntimeMaterialCompiler.h"
#include "Features/DX12NativeScene/RuntimeTextureProgress.h"
#include "Features/DX12NativeScene/TextureSourceBatch.h"
#include "Features/DX12NativeScene/VerifiedTextureSources.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include "Features/NativePBR/InstanceCoverage.h"
#include "Features/NativePBR/NativePBRCompiler.h"
#include "Features/NativePBR/NativePBRRegistry.h"
#include "Features/NativePBR/MaterialService.h"
#include "Features/NativePBR/MaterialDiscovery.h"
#include "Features/NativePBR/MaterialReadiness.h"
#include "Features/NativePBR/VerifiedBridge.h"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <limits>

TEST_CASE("Persistent material cache verifies content and bounds storage", "[native_pbr]")
{
	const auto root = std::filesystem::current_path() / ("native-pbr-cache-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	const std::string first = "e1b03360f3a3c6bd52b4511bc0279f07b38a1b1bb7ab209d31833f7617e2762f";
	const std::string second = "0b6def3dcedfcb01eddc4ff76236225c8f66912ac8346c84e2e1fc1a00e61550";
	struct Cleanup
	{
		std::filesystem::path root;
		std::string first, second;
		~Cleanup()
		{
			std::error_code error;
			std::filesystem::remove(root / (first + ".dds"), error);
			std::filesystem::remove(root / (second + ".dds"), error);
			std::filesystem::remove(root / (first + ".tmp.1.1"), error);
			std::filesystem::remove(root / ".lock", error);
			std::filesystem::remove(root, error);
		}
	} cleanup{ root, first, second };
	DX12Native::MaterialDiskCache cache(root, 128);
	const std::vector<uint8_t> a(128, 42), b(128, 43);
	REQUIRE(cache.Store(first, a) == DX12Native::MaterialDiskCache::StoreResult::Stored);
	REQUIRE(cache.Known(first));
	REQUIRE(cache.Read(first, 128) == a);
	REQUIRE(cache.Store(second, a) == DX12Native::MaterialDiskCache::StoreResult::Unavailable);
	REQUIRE(cache.Read(first, 128) == a);
	REQUIRE(cache.Store(second, b) == DX12Native::MaterialDiskCache::StoreResult::Stored);
	REQUIRE_FALSE(cache.Known(first));
	REQUIRE_FALSE(cache.Read(first, 128));
	REQUIRE(cache.Read(second, 128) == b);
	{
		std::ofstream corrupt(root / (second + ".dds"), std::ios::binary | std::ios::trunc);
		corrupt << "corrupt";
	}
	REQUIRE_FALSE(cache.Read(second, 128));
	REQUIRE_FALSE(cache.Known(second));
	REQUIRE(cache.Store(second, b) == DX12Native::MaterialDiskCache::StoreResult::Stored);
	REQUIRE(cache.Read(second, 128) == b);
	REQUIRE_FALSE(cache.Read("../invalid", 128));
	const auto stale = root / (first + ".tmp.1.1");
	{
		std::ofstream temp(stale, std::ios::binary);
		temp.write(reinterpret_cast<const char*>(a.data()), a.size());
	}
	std::filesystem::last_write_time(stale, std::filesystem::file_time_type::clock::now() - std::chrono::hours(25));
	REQUIRE(cache.Store(first, a) == DX12Native::MaterialDiskCache::StoreResult::Stored);
	REQUIRE_FALSE(std::filesystem::exists(stale));
	REQUIRE(cache.Read(first, 128) == a);
}

TEST_CASE("Material upload publication follows fence and timeout", "[native_pbr]")
{
	using enum DX12Native::MaterialUploadStatus;
	REQUIRE(DX12Native::EvaluateMaterialUpload(0, 4999, 5000) == Pending);
	REQUIRE(DX12Native::EvaluateMaterialUpload(0, 5000, 5000) == Failed);
	REQUIRE(DX12Native::EvaluateMaterialUpload(1, 5000, 5000) == Ready);
	REQUIRE(DX12Native::EvaluateMaterialUpload(UINT64_MAX, 0, 5000) == Failed);
	REQUIRE(DX12Native::MaterialUploadFitsBatch(8, 8, 16));
	REQUIRE_FALSE(DX12Native::MaterialUploadFitsBatch(8, 9, 16));
	REQUIRE_FALSE(DX12Native::MaterialUploadFitsBatch(UINT64_MAX, 1, 16));
	REQUIRE(DX12Native::ClampMaterialBatchMB(0) == 1);
	REQUIRE(DX12Native::ClampMaterialBatchMB(16) == 16);
	REQUIRE(DX12Native::ClampMaterialBatchMB(65) == 64);
}

TEST_CASE("Material upload retries wait for retirement and stop at the deadline", "[native_pbr]")
{
	using enum DX12Native::MaterialUploadStatus;
	DX12Native::MaterialUploadRetry retry;
	retry.Start(100);
	REQUIRE(retry.Poll(false, 5099) == Pending);
	REQUIRE(retry.Waiting());
	REQUIRE(retry.Poll(true, 5100) == Ready);
	REQUIRE_FALSE(retry.Waiting());
	retry.Start(100);
	REQUIRE(retry.Poll(false, 5100) == Failed);
	REQUIRE_FALSE(retry.Waiting());
	retry.Start(200);
	retry.Reset();
	REQUIRE_FALSE(retry.Waiting());
}

TEST_CASE("Texture source batches preserve valid entries after failures", "[native_pbr]")
{
	const std::vector<int> entries{ 1, 2, 3 };
	std::vector<int> loaded;
	unsigned rejected = 0;
	DX12Native::ProcessTextureSources(entries, [&](int entry) {
        if (entry == 2) throw std::runtime_error("missing");
        loaded.push_back(entry); }, [&](const std::exception&) { ++rejected; });
	REQUIRE(loaded == std::vector<int>{ 1, 3 });
	REQUIRE(rejected == 1);
	REQUIRE_THROWS_AS(DX12Native::ProcessTextureSources(entries, [](int) { throw std::bad_alloc(); }, [](const std::exception&) {}), std::bad_alloc);
	REQUIRE_THROWS_AS(DX12Native::ProcessTextureSources(entries, [](int) { throw std::runtime_error("device"); }, [](const std::exception&) { throw; }), std::runtime_error);
}

TEST_CASE("Material page cache transfers ownership and enforces its byte limit", "[native_pbr]")
{
	DX12Native::MaterialPageCache<std::shared_ptr<int>> cache;
	auto first = std::make_shared<int>(1);
	auto second = std::make_shared<int>(2);
	REQUIRE(cache.Retain("first", first, 40, 100));
	REQUIRE(cache.Retain("second", second, 60, 100));
	REQUIRE(cache.Bytes() == 100);
	REQUIRE_FALSE(cache.Retain("third", second, 1, 100));
	REQUIRE_FALSE(cache.Retain("first", first, 1, 101));
	auto reused = cache.Take("first");
	REQUIRE(reused);
	REQUIRE(*reused == first);
	REQUIRE(cache.Bytes() == 60);
	REQUIRE_FALSE(cache.Take("missing"));
	cache.Clear();
	REQUIRE(cache.Bytes() == 0);
	REQUIRE(**reused == 1);
	REQUIRE(*second == 2);
}

TEST_CASE("Material page cache finds and transfers pages by individual slice", "[native_pbr]")
{
	DX12Native::MaterialPageCache<std::shared_ptr<int>> cache;
	auto page = std::make_shared<int>(7);
	REQUIRE(cache.Retain("page", page, 40, 100, { "albedo", "normal", "rmaos" }));
	auto found = cache.TakeContaining("normal");
	REQUIRE(found);
	REQUIRE(found->key == "page");
	REQUIRE(found->page == page);
	REQUIRE(found->slices == std::vector<std::string>{ "albedo", "normal", "rmaos" });
	REQUIRE(cache.Bytes() == 0);
	REQUIRE_FALSE(cache.TakeContaining("albedo"));
	REQUIRE_FALSE(cache.TakeContaining("normal"));
	REQUIRE_FALSE(cache.TakeContaining("rmaos"));
}

TEST_CASE("Native PBR invalidation and recreation are observable", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	NativePBR::MaterialData material;
	registry.Publish(&source, registry.BeginCapture(&source), material);
	const auto first = registry.Export().front();
	const auto published = registry.GetStatistics().revision;
	registry.MarkDirty(&source);
	REQUIRE(registry.Export().empty());
	const auto dirty = registry.GetStatistics().revision;
	REQUIRE(dirty > published);
	registry.MarkDirty(&source);
	REQUIRE(registry.GetStatistics().revision == dirty);
	registry.Publish(&source, registry.BeginCapture(&source), material);
	REQUIRE(registry.GetStatistics().revision > dirty);
	REQUIRE(registry.Export().front().revision == first.revision);
	registry.Forget(&source);
	registry.Publish(&source, registry.BeginCapture(&source), material);
	REQUIRE(registry.Export().front().identity != first.identity);
}

TEST_CASE("PBR recipes retain observed texture capabilities", "[native_pbr]")
{
	NativePBR::Snapshot snapshot;
	snapshot.identity = 4;
	snapshot.revision = 7;
	auto& observed = snapshot.material.textureResources[0];
	observed = { 1024, 512, 11, 1, 98, 0x800, 8, 0, 1, true };
	const auto recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(recipe.textures[0].resource == observed);
	REQUIRE_FALSE(recipe.textures[1].resource.texture2D);
	REQUIRE(recipe.issues & NativePBR::UnmatchedSceneInstance);
}

TEST_CASE("Native PBR bulk invalidation reports only state changes", "[native_pbr]")
{
	NativePBR::Registry registry;
	int first = 0, second = 0;
	registry.Publish(&first, registry.BeginCapture(&first), {});
	registry.Publish(&second, registry.BeginCapture(&second), {});
	registry.MarkAllDirty();
	const auto invalidated = registry.GetStatistics();
	REQUIRE(invalidated.dirty == 2);
	REQUIRE(registry.Export().empty());
	registry.MarkAllDirty();
	REQUIRE(registry.GetStatistics().revision == invalidated.revision);
}

TEST_CASE("Native PBR rejects stale captures", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	auto capture = registry.BeginCapture(&source);
	registry.MarkDirty(&source);
	REQUIRE_FALSE(registry.Publish(&source, capture, {}));
	capture = registry.BeginCapture(&source);
	registry.Forget(&source);
	auto replacement = registry.BeginCapture(&source);
	REQUIRE_FALSE(registry.Publish(&source, capture, {}));
	REQUIRE(registry.Publish(&source, replacement, {}));
	REQUIRE(registry.Export().front().identity == replacement.identity);
}

TEST_CASE("Landscape texture identity changes revise the snapshot", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	NativePBR::MaterialData material;
	material.kind = NativePBR::MaterialKind::Landscape;
	material.landscapeTileCount = 1;
	material.landscapeTiles[0].textureNames[0] = "textures/terrain/a.dds";
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), material));
	const auto before = registry.Export().front();
	registry.MarkDirty(&source);
	material.landscapeTiles[0].textureNames[0] = "textures/terrain/b.dds";
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), material));
	const auto after = registry.Export().front();
	REQUIRE(after.identity == before.identity);
	REQUIRE(after.revision > before.revision);
	REQUIRE(after.material.landscapeTiles[0].textureNames[0] == "textures/terrain/b.dds");
}

TEST_CASE("Registry exports consistent changed generations", "[native_pbr]")
{
	NativePBR::Registry registry;
	REQUIRE_FALSE(registry.ExportIfChanged(0));
	int source = 0;
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	auto first = registry.ExportIfChanged(0);
	REQUIRE(first);
	REQUIRE(first->snapshots.size() == 1);
	REQUIRE_FALSE(registry.ExportIfChanged(first->revision));
	registry.MarkDirty(&source);
	auto pending = registry.ExportIfChanged(first->revision);
	REQUIRE(pending);
	REQUIRE(pending->snapshots.empty());
	REQUIRE(pending->pendingIdentities == std::vector<uint64_t>{ first->snapshots.front().identity });
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	auto restored = registry.ExportIfChanged(pending->revision);
	REQUIRE(restored);
	REQUIRE(restored->snapshots.front().revision == first->snapshots.front().revision);
}

TEST_CASE("Registry generations distinguish pending and removed identities", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	const auto capture = registry.BeginCapture(&source);
	auto pending = registry.ExportIfChanged(0);
	REQUIRE(pending);
	REQUIRE(pending->pendingIdentities == std::vector<uint64_t>{ capture.identity });
	REQUIRE(registry.GetStatistics().mesh == 0);
	registry.Forget(&source);
	auto removed = registry.ExportIfChanged(pending->revision);
	REQUIRE(removed);
	REQUIRE(removed->pendingIdentities.empty());
	REQUIRE(removed->snapshots.empty());
}

TEST_CASE("Registry leaves invalid snapshots pending", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	auto capture = registry.BeginCapture(&source);
	NativePBR::MaterialData material;
	material.uvOffsetScale[0][0] = std::numeric_limits<float>::quiet_NaN();
	REQUIRE_FALSE(registry.Publish(&source, capture, material));
	REQUIRE(registry.Export().empty());
	REQUIRE(registry.GetStatistics().dirty == 1);
	material.uvOffsetScale[0][0] = 0;
	material.landscapeTileCount = 7;
	REQUIRE_FALSE(registry.Publish(&source, capture, material));
	material.landscapeTileCount = 0;
	REQUIRE(registry.Publish(&source, capture, material));
}

TEST_CASE("Native recipes retain raw values and expose unsupported data", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 3, 7, {} };
	snapshot.material.roughnessScale = 0.35f;
	auto recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(recipe.identity == 3);
	REQUIRE(recipe.parameters.roughnessScale == 0.35f);
	REQUIRE(recipe.issues & NativePBR::MissingRequiredTextures);
	REQUIRE(recipe.issues & NativePBR::UnknownOpacity);
	REQUIRE(recipe.issues & NativePBR::UnknownNormalMode);
	REQUIRE(recipe.issues & NativePBR::UnknownUvAnimation);
	REQUIRE(recipe.issues & NativePBR::UnmatchedSceneInstance);
	snapshot.material.texturePathsComplete = true;
	snapshot.material.texturePaths[0] = "base";
	snapshot.material.texturePaths[1] = "normal";
	snapshot.material.texturePaths[2] = "rmaos";
	snapshot.material.pbrFlags = 1;
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE_FALSE(recipe.issues & NativePBR::MissingRequiredTextures);
	REQUIRE(recipe.issues & NativePBR::UnsupportedPbrFeatures);
	snapshot.material.kind = NativePBR::MaterialKind::Landscape;
	REQUIRE(NativePBR::CompileRecipe(snapshot).issues & NativePBR::UnsupportedLandscape);
}

TEST_CASE("Recipe cache updates changed identities and retains pending generations", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	int first = 0, second = 0;
	REQUIRE(registry.Publish(&first, registry.BeginCapture(&first), {}));
	REQUIRE(registry.Publish(&second, registry.BeginCapture(&second), {}));
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE(cache.Compilations() == 2);
	registry.MarkDirty(&first);
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE(cache.Pending() == 1);
	REQUIRE(cache.Recipes().size() == 2);
	REQUIRE(cache.Compilations() == 2);
	NativePBR::MaterialData changed;
	changed.roughnessScale = 0.5f;
	REQUIRE(registry.Publish(&first, registry.BeginCapture(&first), changed));
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE(cache.Compilations() == 3);
	registry.Forget(&second);
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE(cache.Recipes().size() == 1);
	REQUIRE(cache.Compilations() == 3);
}

TEST_CASE("Resource identity changes revise material recipes", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	int source = 0;
	NativePBR::MaterialData data;
	data.texturePaths[0] = "textures/base.dds";
	data.textureResourceNames[0] = "textures/seasonal.dds";
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), data));
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE(cache.Recipes().begin()->second.parameters.textureResourceNames[0] == "textures/seasonal.dds");
	registry.MarkDirty(&source);
	data.textureResourceNames[0] = "textures/winter.dds";
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), data));
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE(cache.Compilations() == 2);
	REQUIRE(cache.Recipes().begin()->second.parameters.texturePaths[0] == "textures/base.dds");
	REQUIRE(cache.Recipes().begin()->second.parameters.textureResourceNames[0] == "textures/winter.dds");
}

TEST_CASE("Recipe cache rejects stale generations", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	int source = 0;
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	auto oldGeneration = *registry.ExportIfChanged(0);
	cache.Update(oldGeneration);
	registry.Forget(&source);
	auto removed = *registry.ExportIfChanged(cache.Revision());
	cache.Update(removed);
	REQUIRE(cache.Recipes().empty());
	cache.Update(oldGeneration);
	REQUIRE(cache.Recipes().empty());
	REQUIRE(cache.Revision() == removed.revision);
}

TEST_CASE("Recipe cache exposes retained pending identities", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	int source = 0;
	auto capture = registry.BeginCapture(&source);
	REQUIRE(registry.Publish(&source, capture, {}));
	cache.Update(*registry.ExportIfChanged(0));
	REQUIRE_FALSE(cache.IsPending(capture.identity));
	registry.MarkDirty(&source);
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE(cache.IsPending(capture.identity));
	REQUIRE(cache.Recipes().contains(capture.identity));
	registry.Forget(&source);
	cache.Update(*registry.ExportIfChanged(cache.Revision()));
	REQUIRE_FALSE(cache.IsPending(capture.identity));
	REQUIRE_FALSE(cache.Recipes().contains(capture.identity));
}

TEST_CASE("Native recipes reject invalid sampler and category values", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 1, 1, {} };
	snapshot.material.clampMode = 4;
	REQUIRE(NativePBR::CompileRecipe(snapshot).issues & NativePBR::InvalidSnapshot);
	snapshot.material.clampMode = 3;
	snapshot.material.kind = static_cast<NativePBR::MaterialKind>(255);
	REQUIRE(NativePBR::CompileRecipe(snapshot).issues & NativePBR::InvalidSnapshot);
}

TEST_CASE("Observed base parameter changes invalidate only the affected snapshot", "[native_pbr]")
{
	NativePBR::Registry registry;
	int first = 0, second = 0;
	NativePBR::MaterialData data;
	REQUIRE(registry.Publish(&first, registry.BeginCapture(&first), data));
	REQUIRE(registry.Publish(&second, registry.BeginCapture(&second), data));
	auto revision = registry.GetStatistics().revision;
	registry.ObserveBase(&first, data.uvOffsetScale, data.materialAlpha, data.clampMode);
	REQUIRE(registry.GetStatistics().revision == revision);
	data.uvOffsetScale[1][2] = 2.0f;
	registry.ObserveBase(&first, data.uvOffsetScale, data.materialAlpha, data.clampMode);
	REQUIRE(registry.GetStatistics().revision > revision);
	REQUIRE(registry.Export().size() == 1);
	REQUIRE(registry.Publish(&first, registry.BeginCapture(&first), data));
	REQUIRE(registry.Export().size() == 2);
}

TEST_CASE("Native recipes flag additional unsupported effects", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 1, 1, {} };
	snapshot.material.texturePaths[static_cast<size_t>(NativePBR::TextureRole::Displacement)] = "displacement";
	snapshot.material.texturePaths[static_cast<size_t>(NativePBR::TextureRole::Features0)] = "features";
	snapshot.material.projectedRoughness = 0.5f;
	const auto recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(recipe.issues & NativePBR::UnsupportedDisplacement);
	REQUIRE(recipe.issues & NativePBR::UnsupportedFeatureTextures);
	REQUIRE(recipe.issues & NativePBR::UnsupportedProjection);
}

TEST_CASE("Texture replacement invalidates the affected material", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0, firstTexture = 0, secondTexture = 0;
	std::array<const void*, static_cast<size_t>(NativePBR::TextureRole::Count) + 1> textures{};
	textures[0] = &firstTexture;
	registry.ObserveTextures(&source, textures);
	auto capture = registry.BeginCapture(&source);
	REQUIRE(registry.Publish(&source, capture, {}));
	const auto revision = registry.GetStatistics().revision;
	registry.ObserveTextures(&source, textures);
	REQUIRE(registry.GetStatistics().revision == revision);
	textures[0] = &secondTexture;
	registry.ObserveTextures(&source, textures);
	REQUIRE(registry.GetStatistics().revision > revision);
	REQUIRE(registry.Export().empty());
	auto stale = registry.BeginCapture(&source);
	textures[0] = &firstTexture;
	registry.ObserveTextures(&source, textures);
	REQUIRE_FALSE(registry.Publish(&source, stale, {}));
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
}

TEST_CASE("Landscape layer texture replacement invalidates its snapshot", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0, texture = 0;
	std::array<const void*, 24> textures{};
	registry.ObserveLandscapeTextures(&source, textures);
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	const auto before = registry.GetStatistics().revision;
	registry.ObserveLandscapeTextures(&source, textures);
	REQUIRE(registry.GetStatistics().revision == before);
	textures[23] = &texture;
	registry.ObserveLandscapeTextures(&source, textures);
	REQUIRE(registry.GetStatistics().revision > before);
	REQUIRE(registry.Export().empty());
}

TEST_CASE("A base parameter change rejects a pending capture", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	NativePBR::MaterialData data;
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), data));
	registry.ObserveBase(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode);
	registry.MarkDirty(&source);
	auto capture = registry.BeginCapture(&source);
	data.materialAlpha = 0.5f;
	registry.ObserveBase(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode);
	REQUIRE_FALSE(registry.Publish(&source, capture, data));
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), data));
}

TEST_CASE("Mesh recipe texture requests retain roles and interpretation", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 1, 1, {} };
	snapshot.material.texturePaths[0] = "base.dds";
	snapshot.material.textureResourceNames[0] = "observed.dds";
	auto recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(recipe.textures[0].role == NativePBR::TextureRole::BaseColor);
	REQUIRE(recipe.textures[0].interpretation == NativePBR::TextureInterpretation::ColorSrgb);
	REQUIRE(recipe.textures[0].assetPath == "base.dds");
	REQUIRE(recipe.textures[0].observedResourceName == "observed.dds");
	REQUIRE(recipe.textures[2].required);
	REQUIRE(recipe.textures[2].interpretation == NativePBR::TextureInterpretation::DataLinear);
	REQUIRE(recipe.textures[2].channels == NativePBR::TextureChannels::RoughnessMetallicAoSpecular);
	REQUIRE(recipe.textures[0].channels == NativePBR::TextureChannels::Unspecified);
	REQUIRE_FALSE(recipe.textures[3].required);
	REQUIRE(recipe.textures[3].interpretation == NativePBR::TextureInterpretation::ColorSrgb);
}

TEST_CASE("Texture-set replacement invalidates unchanged texture objects", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0, textureSet = 0;
	std::array<const void*, static_cast<size_t>(NativePBR::TextureRole::Count) + 1> textures{};
	registry.ObserveTextures(&source, textures);
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	const auto revision = registry.GetStatistics().revision;
	textures.back() = &textureSet;
	registry.ObserveTextures(&source, textures);
	REQUIRE(registry.GetStatistics().revision > revision);
	REQUIRE(registry.Export().empty());
}

TEST_CASE("Combined observation captures only changed material generations", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0, texture = 0;
	NativePBR::MaterialData data;
	std::array<const void*, 8> textures{};
	auto capture = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures);
	REQUIRE(capture);
	REQUIRE(registry.Publish(&source, capture, data));
	REQUIRE_FALSE(registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures));
	textures[0] = &texture;
	capture = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures);
	REQUIRE(capture);
	data.materialAlpha = 0.5f;
	auto next = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures);
	REQUIRE_FALSE(registry.Publish(&source, capture, data));
	REQUIRE(registry.Publish(&source, next, data));
}

TEST_CASE("Publication rejects a snapshot inconsistent with its observation", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	NativePBR::MaterialData data;
	std::array<const void*, 8> textures{};
	auto capture = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures);
	data.materialAlpha = 0.5f;
	REQUIRE_FALSE(registry.Publish(&source, capture, data));
	REQUIRE(registry.Export().empty());
	capture = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures);
	REQUIRE(registry.Publish(&source, capture, data));
}

TEST_CASE("Renderer resource replacement invalidates the material generation", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::MaterialData data;
	int source = 0, renderer = 0, first = 0, second = 0;
	std::array<const void*, 8> textures{};
	std::array<const void*, 14> resources{};
	resources[0] = &renderer;
	resources[1] = &first;
	auto capture = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures, resources);
	REQUIRE(registry.Publish(&source, capture, data));
	const auto original = registry.Export().front();
	REQUIRE_FALSE(registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures, resources));
	resources[1] = &second;
	const auto replacement = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures, resources);
	REQUIRE(replacement);
	REQUIRE_FALSE(registry.Publish(&source, capture, data));
	REQUIRE(registry.Publish(&source, replacement, data));
	const auto updated = registry.Export().front();
	REQUIRE(updated.identity == original.identity);
	REQUIRE(updated.revision > original.revision);
	REQUIRE(updated.material.textureGeneration > original.material.textureGeneration);
}

TEST_CASE("A resource change during capture rejects the old snapshot", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::MaterialData data;
	int source = 0, first = 0, second = 0;
	std::array<const void*, 8> textures{};
	std::array<const void*, 14> resources{};
	resources[1] = &first;
	const auto before = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures, resources);
	resources[1] = &second;
	const auto after = registry.ObserveAndBegin(&source, data.uvOffsetScale, data.materialAlpha, data.clampMode, textures, resources);
	REQUIRE_FALSE(registry.Publish(&source, before, data));
	REQUIRE(registry.Export().empty());
	REQUIRE(registry.Publish(&source, after, data));
	REQUIRE(registry.Export().front().material.textureGeneration == after.textureGeneration);
}

TEST_CASE("Incremental registry export copies only changed snapshots", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	int first = 0, second = 0;
	REQUIRE(registry.Publish(&first, registry.BeginCapture(&first), {}));
	REQUIRE(registry.Publish(&second, registry.BeginCapture(&second), {}));
	auto initial = registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	REQUIRE(initial->changed.size() == 2);
	cache.Update(*initial);
	registry.MarkDirty(&first);
	auto pending = registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	REQUIRE(pending->changed.empty());
	REQUIRE(pending->pendingIdentities.size() == 1);
	cache.Update(*pending);
	NativePBR::MaterialData updated;
	updated.roughnessScale = 0.25f;
	REQUIRE(registry.Publish(&first, registry.BeginCapture(&first), updated));
	auto changed = registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	REQUIRE(changed->changed.size() == 1);
	cache.Update(*changed);
	REQUIRE(cache.Compilations() == 3);
	registry.Forget(&second);
	auto removed = registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	REQUIRE(removed->changed.empty());
	cache.Update(*removed);
	REQUIRE(cache.Recipes().size() == 1);
}

TEST_CASE("Incremental cache recovers pending and recreated materials", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	int source = 0;
	NativePBR::MaterialData data;
	auto first = registry.BeginCapture(&source);
	auto initialPending = registry.ExportChanges(0, cache.KnownRevisions());
	REQUIRE(initialPending->pendingIdentities.size() == 1);
	cache.Update(*initialPending);
	REQUIRE(cache.Recipes().empty());
	REQUIRE(registry.Publish(&source, first, data));
	cache.Update(*registry.ExportChanges(cache.Revision(), cache.KnownRevisions()));
	REQUIRE(cache.Recipes().contains(first.identity));
	const auto compiled = cache.Compilations();
	registry.MarkDirty(&source);
	cache.Update(*registry.ExportChanges(cache.Revision(), cache.KnownRevisions()));
	REQUIRE(cache.IsPending(first.identity));
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), data));
	auto restored = registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	REQUIRE(restored->changed.empty());
	cache.Update(*restored);
	REQUIRE_FALSE(cache.IsPending(first.identity));
	REQUIRE(cache.Compilations() == compiled);
	registry.Forget(&source);
	auto replacement = registry.BeginCapture(&source);
	REQUIRE(replacement.identity != first.identity);
	REQUIRE(registry.Publish(&source, replacement, data));
	auto recreated = registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	REQUIRE(recreated->changed.size() == 1);
	cache.Update(*recreated);
	REQUIRE_FALSE(cache.Recipes().contains(first.identity));
	REQUIRE(cache.Recipes().contains(replacement.identity));
}

TEST_CASE("Published recipes exclude pending materials and preserve immutable versions", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	int first = 0, second = 0;
	auto firstCapture = registry.BeginCapture(&first);
	auto secondCapture = registry.BeginCapture(&second);
	REQUIRE(registry.Publish(&first, firstCapture, {}));
	REQUIRE(registry.Publish(&second, secondCapture, {}));
	auto changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1);
	cache.Update(changes);
	auto partial = NativePBR::PublishRecipes(cache, changes, {});
	REQUIRE_FALSE(partial->batchesComplete);
	REQUIRE(partial->recipes.size() == 1);
	changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1);
	cache.Update(changes);
	auto complete = NativePBR::PublishRecipes(cache, changes, partial);
	REQUIRE(complete->batchesComplete);
	REQUIRE(complete->recipes.size() == 2);
	REQUIRE(complete->recipes.at(partial->recipes.begin()->first) == partial->recipes.begin()->second);
	registry.MarkDirty(&first);
	changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	cache.Update(changes);
	auto pending = NativePBR::PublishRecipes(cache, changes, complete);
	REQUIRE_FALSE(pending->recipes.contains(firstCapture.identity));
	REQUIRE(pending->recipes.at(secondCapture.identity) == complete->recipes.at(secondCapture.identity));
	REQUIRE(registry.Publish(&first, registry.BeginCapture(&first), {}));
	changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	cache.Update(changes);
	auto restored = NativePBR::PublishRecipes(cache, changes, pending);
	REQUIRE(restored->recipes.contains(firstCapture.identity));
	registry.Forget(&first);
	changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	cache.Update(changes);
	auto removed = NativePBR::PublishRecipes(cache, changes, restored);
	REQUIRE_FALSE(removed->recipes.contains(firstCapture.identity));
	REQUIRE(complete->recipes.contains(firstCapture.identity));
}

TEST_CASE("Incremental export bounds material compilation per update", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	std::array<int, 5> sources{};
	for (auto& source : sources) REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	auto first = registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 2);
	REQUIRE(first->changed.size() == 2);
	REQUIRE(first->hasMore);
	cache.Update(*first);
	REQUIRE(cache.Revision() == 0);
	auto second = registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 2);
	REQUIRE(second->changed.size() == 2);
	cache.Update(*second);
	auto last = registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 2);
	REQUIRE(last->changed.size() == 1);
	REQUIRE_FALSE(last->hasMore);
	cache.Update(*last);
	REQUIRE(cache.Revision() == last->revision);
	REQUIRE(cache.Compilations() == 5);
	REQUIRE_FALSE(registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 2));
}

TEST_CASE("Partial recipe updates reject stale full exports and duplicate work", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	std::array<int, 3> sources{};
	REQUIRE(registry.Publish(&sources[0], registry.BeginCapture(&sources[0]), {}));
	auto old = *registry.ExportIfChanged(0);
	REQUIRE(registry.Publish(&sources[1], registry.BeginCapture(&sources[1]), {}));
	REQUIRE(registry.Publish(&sources[2], registry.BeginCapture(&sources[2]), {}));
	auto partial = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1);
	REQUIRE(partial.hasMore);
	cache.Update(partial);
	REQUIRE(cache.Compilations() == 1);
	cache.Update(partial);
	REQUIRE(cache.Compilations() == 1);
	cache.Update(old);
	REQUIRE(cache.Compilations() == 1);
	REQUIRE(cache.Revision() == 0);
}

TEST_CASE("Deferred changed recipes remain pending until compiled", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	std::array<int, 3> sources{};
	for (auto& source : sources) REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	auto first = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1);
	REQUIRE(first.hasMore);
	REQUIRE(first.pendingIdentities.size() == 2);
	cache.Update(first);
	REQUIRE(cache.Pending() == 2);
	auto second = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1);
	cache.Update(second);
	REQUIRE(cache.Pending() == 1);
	auto last = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1);
	cache.Update(last);
	REQUIRE(cache.Pending() == 0);
	REQUIRE(cache.Recipes().size() == 3);
}

TEST_CASE("Bounded export advances when the registry changes mid-batch", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	std::array<int, 3> sources{};
	for (auto& source : sources) REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	auto first = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 0);
	REQUIRE(first.changed.size() == 1);
	REQUIRE(first.hasMore);
	cache.Update(first);
	registry.Forget(&sources[0]);
	auto next = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1);
	REQUIRE(next.revision > first.revision);
	cache.Update(next);
	while (auto more = registry.ExportChanges(cache.Revision(), cache.KnownRevisions(), 1)) cache.Update(*more);
	REQUIRE(cache.Recipes().size() == 2);
	REQUIRE(cache.Pending() == 0);
	cache.Update(first);
	REQUIRE(cache.Recipes().size() == 2);
}

TEST_CASE("Instance hints preserve many-to-one associations and expire with the material", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	registry.ObserveInstance(&source, "000123:Example.esm", "ShapeA");
	registry.ObserveInstance(&source, "000124:Example.esm", "ShapeA");
	registry.ObserveInstance(&source, "000123:Example.esm", "ShapeA");
	const auto hints = registry.ExportInstanceHints();
	REQUIRE(hints.size() == 2);
	REQUIRE(hints[0].materialIdentity == hints[1].materialIdentity);
	REQUIRE(registry.GetStatistics().instanceHints == 2);
	registry.Forget(&source);
	REQUIRE(registry.ExportInstanceHints().empty());
}

TEST_CASE("Instance hint limits release capacity on material destruction", "[native_pbr]")
{
	NativePBR::Registry registry;
	std::array<int, 65> sources{};
	for (size_t source = 0; source < 64; ++source)
		for (size_t hint = 0; hint < 64; ++hint)
			registry.ObserveInstance(&sources[source], std::to_string(source), std::to_string(hint));
	REQUIRE(registry.GetStatistics().instanceHints == NativePBR::Registry::kMaximumInstanceHints);
	registry.ObserveInstance(&sources[64], "new", "shape");
	REQUIRE(registry.GetStatistics().instanceHints == NativePBR::Registry::kMaximumInstanceHints);
	REQUIRE(registry.GetStatistics().hintLimitHits == 1);
	registry.Forget(&sources[0]);
	registry.ObserveInstance(&sources[64], "new", "shape");
	REQUIRE(registry.GetStatistics().instanceHints == NativePBR::Registry::kMaximumInstanceHints - 63);
}

TEST_CASE("Prepared instance coverage keeps ambiguous candidates separate", "[native_pbr]")
{
	std::vector<NativePBR::PreparedInstanceKey> prepared{ { "01:Mod.esm", "A" }, { "02:Mod.esm", "B" },
		{ "02:Mod.esm", "B" }, { "03:Mod.esm", "C" }, { "", "" }, { "04:Mod.esm", "D" } };
	std::vector<NativePBR::InstanceHint> hints{ { 1, "01:mod.esm", "a" }, { 2, "02:mod.esm", "B" },
		{ 3, "04:mod.esm", "D" }, { 4, "04:mod.esm", "D" } };
	const auto coverage = NativePBR::AnalyzeInstanceCoverage(prepared, hints);
	REQUIRE(coverage.total == 6);
	REQUIRE(coverage.uniqueCandidates == 1);
	REQUIRE(coverage.ambiguous == 3);
	REQUIRE(coverage.noObservation == 1);
	REQUIRE(coverage.missingIdentity == 1);
}

TEST_CASE("Prepared candidate analysis counts only published recipes for unique candidates", "[native_pbr]")
{
	std::vector<NativePBR::PreparedInstanceKey> prepared{ { "a", "shape" }, { "b", "shape" }, { "c", "shape" } };
	std::vector<NativePBR::InstanceHint> hints{ { 1, "a", "shape" }, { 2, "b", "shape" },
		{ 3, "c", "shape" }, { 4, "c", "shape" } };
	NativePBR::RecipeGeneration recipes;
	recipes.revision = 42;
	recipes.recipes.emplace(1, std::make_shared<const NativePBR::Recipe>());
	recipes.recipes.emplace(3, std::make_shared<const NativePBR::Recipe>());
	const auto coverage = NativePBR::AnalyzeInstanceCoverage(prepared, hints, &recipes);
	REQUIRE(coverage.uniqueCandidates == 2);
	REQUIRE(coverage.ambiguous == 1);
	REQUIRE(coverage.recipeGenerationAvailable);
	REQUIRE(coverage.recipeRevision == 42);
	REQUIRE(coverage.candidateRecipes == 1);
	REQUIRE(coverage.candidateRecipesMissing == 1);
	REQUIRE_FALSE(NativePBR::AnalyzeInstanceCoverage(prepared, hints).recipeGenerationAvailable);
}

TEST_CASE("Conflicting instance properties prevent unique candidate matching", "[native_pbr]")
{
	NativePBR::Registry registry;
	int material = 0;
	NativePBR::InstanceProperties first;
	first.alphaProperty = true;
	first.alphaFlags = 512;
	first.alphaThreshold = 128;
	first.emissionKnown = true;
	first.emissiveColor = { 1.0f, 0.5f, 0.25f };
	first.emissiveMultiplier = 2.0f;
	registry.ObserveInstance(&material, "ref", "shape", first);
	auto hints = registry.ExportInstanceHints();
	REQUIRE(hints.size() == 1);
	REQUIRE(hints.front().properties == first);
	REQUIRE(NativePBR::AnalyzeInstanceCoverage({ { "ref", "shape" } }, hints).uniqueCandidates == 1);
	auto second = first;
	second.alphaThreshold = 64;
	registry.ObserveInstance(&material, "ref", "shape", second);
	hints = registry.ExportInstanceHints();
	REQUIRE(hints.front().propertiesConflict);
	REQUIRE(NativePBR::AnalyzeInstanceCoverage({ { "ref", "shape" } }, hints).ambiguous == 1);
	const auto revision = registry.Revision();
	registry.ObserveInstance(&material, "ref", "shape", first);
	registry.ObserveInstance(&material, "ref", "shape", second);
	REQUIRE(registry.Revision() == revision);
}

TEST_CASE("Clearing scene observations preserves published materials", "[native_pbr]")
{
	NativePBR::Registry registry;
	int source = 0;
	REQUIRE(registry.Publish(&source, registry.BeginCapture(&source), {}));
	const auto material = registry.Export().front();
	registry.ObserveInstance(&source, "000123:Example.esm", "Shape");
	const auto before = registry.GetStatistics().revision;
	registry.ClearInstanceHints();
	REQUIRE(registry.GetStatistics().revision > before);
	REQUIRE(registry.ExportInstanceHints().empty());
	REQUIRE(registry.Export().front().identity == material.identity);
	REQUIRE(registry.Export().front().revision == material.revision);
	const auto cleared = registry.GetStatistics().revision;
	registry.ClearInstanceHints();
	REQUIRE(registry.GetStatistics().revision == cleared);
}

TEST_CASE("Logical texture identities remain stable while retained", "[native_pbr]")
{
	DX12Native::LogicalTextureRegistry registry(2);
	DX12Native::TextureIdentity first{ "digest-a", 1024, 1024, 7, 11 };
	DX12Native::TextureIdentity second{ "digest-b", 1024, 1024, 7, 11 };
	DX12Native::TextureIdentity third{ "digest-c", 1024, 1024, 7, 11 };
	auto a = registry.Acquire(first);
	auto same = registry.Acquire(first);
	auto b = registry.Acquire(second);
	REQUIRE(a->handle == same->handle);
	REQUIRE(a->handle != b->handle);
	REQUIRE_FALSE(registry.Acquire(third));
	const auto old = a->handle;
	a.reset();
	same.reset();
	auto c = registry.Acquire(third);
	REQUIRE(c);
	auto recreated = registry.Acquire(first);
	REQUIRE_FALSE(recreated);
	b.reset();
	recreated = registry.Acquire(first);
	REQUIRE(recreated);
	REQUIRE(recreated->handle != old);
}

TEST_CASE("DXGI budget limits new material allocations without double counting", "[native_pbr]")
{
	using DX12Native::VideoMemoryBudget;
	constexpr auto mib = VideoMemoryBudget::kMiB;
	const auto ample = VideoMemoryBudget::Evaluate(8192 * mib, 6144 * mib, 0, 512 * mib);
	REQUIRE(ample.dxgiAvailable);
	REQUIRE(ample.effectiveBytes == 512 * mib);
	const auto pressured = VideoMemoryBudget::Evaluate(8192 * mib, 8000 * mib, 0, 512 * mib);
	REQUIRE(pressured.effectiveBytes == 0);
	const auto retained = VideoMemoryBudget::Evaluate(8192 * mib, 8000 * mib, 128 * mib, 512 * mib);
	REQUIRE(retained.effectiveBytes == 128 * mib);
	const auto unavailable = VideoMemoryBudget::Evaluate(0, 0, 0, 512 * mib);
	REQUIRE_FALSE(unavailable.dxgiAvailable);
	REQUIRE(unavailable.effectiveBytes == 512 * mib);
}

TEST_CASE("Recipe texture paths stay relative and reject traversal", "[native_pbr]")
{
	REQUIRE(NativePBR::CanonicalTexturePath("Textures\\Armor\\Body.DDS") == "textures/armor/body.dds");
	REQUIRE(NativePBR::CanonicalTexturePath("armor/body.dds") == "textures/armor/body.dds");
	REQUIRE_FALSE(NativePBR::CanonicalTexturePath("../outside.dds"));
	REQUIRE_FALSE(NativePBR::CanonicalTexturePath("C:\\outside.dds"));
	REQUIRE_FALSE(NativePBR::CanonicalTexturePath("/outside.dds"));
	NativePBR::Snapshot snapshot{ 1, 1, {} };
	snapshot.material.texturePaths[0] = "../outside.dds";
	const auto recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(recipe.issues & NativePBR::InvalidTexturePath);
	REQUIRE(recipe.textures[0].canonicalPath.empty());
	REQUIRE(recipe.textures[0].assetPath == "../outside.dds");
}

TEST_CASE("Strict native bridge rejects ambiguous and incompatible live source matches", "[native_pbr]")
{
	NativePBR::Registry registry;
	int material = 0;
	auto data = NativePBR::MaterialData{};
	data.texturePaths = { "textures/a.dds", "textures/n.dds", "textures/r.dds", "", "", "", "" };
	data.texturePathsComplete = true;
	const auto token = registry.BeginCapture(&material);
	REQUIRE(registry.Publish(&material, token, data));
	auto changes = registry.ExportChanges(0, {});
	REQUIRE(changes);
	NativePBR::RecipeCache cache;
	cache.Update(*changes);
	const auto generation = NativePBR::PublishRecipes(cache, *changes, {});
	REQUIRE(generation->batchesComplete);
	NativePBR::PreparedMatchInput offline;
	offline.referenceKey = "A:Skyrim.esm";
	offline.shapeName = "Cube";
	offline.valid = offline.pbr = offline.opaque = true;
	offline.texturePaths = { "textures/a.dds", "textures/n.dds", "textures/r.dds", "" };
	offline.uvOffsetScale = { 0, 0, 1, 1 };
	offline.alphaFlags = 0;
	NativePBR::InstanceHint hint;
	hint.materialIdentity = token.identity;
	hint.referenceKey = offline.referenceKey;
	hint.shapeName = offline.shapeName;
	hint.properties.alphaFlags = offline.alphaFlags;
	NativePBR::LiveBindingSource source{ generation, { hint } };
	auto selection = NativePBR::ResolveLiveMatches({ offline }, source);
	REQUIRE(selection.report.matched == 1);
	REQUIRE(selection.matches[0]);
	REQUIRE(selection.matches[0]->recipe->identity == token.identity);
	source.hints.push_back(hint);
	REQUIRE(NativePBR::ResolveLiveMatches({ offline }, source).report.ambiguous == 1);
	source.hints.pop_back();
	offline.texturePaths[1] = "textures/other.dds";
	REQUIRE(NativePBR::ResolveLiveMatches({ offline }, source).report.incompatible == 1);
	offline.texturePaths[1] = "textures/n.dds";
	offline.alpha = 0.5f;
	REQUIRE(NativePBR::ResolveLiveMatches({ offline }, source).report.incompatible == 1);
	offline.alpha = 1.0f;
	hint.properties.alphaProperty = true;
	source.hints = { hint };
	REQUIRE(NativePBR::ResolveLiveMatches({ offline }, source).report.incompatible == 1);
	source.hints = {};
	REQUIRE(NativePBR::ResolveLiveMatches({ offline }, source).report.missing == 1);
	source.hints = { hint };
	REQUIRE(NativePBR::ResolveLiveMatches({ offline, offline }, source).report.ambiguous == 2);
}

TEST_CASE("Verified resident DDS sources reject path collisions and incompatible live texture dimensions", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 1, 1, {} };
	snapshot.material.texturePaths = { "textures/a.dds", "textures/n.dds", "textures/r.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	auto recipe = NativePBR::CompileRecipe(snapshot);
	DX12Native::VerifiedTextureSources sources;
	const std::string a(64, 'a'), b(64, 'b');
	REQUIRE(sources.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Missing);
	REQUIRE(sources.Observe("Textures\\A.DDS", a, 256, 256, 8));
	REQUIRE(sources.Observe("textures/n.dds", a, 256, 256, 8));
	REQUIRE(sources.Observe("textures/r.dds", b, 256, 256, 8));
	REQUIRE(sources.Size() == 3);
	REQUIRE(sources.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Available);
	REQUIRE(sources.Observe("textures/a.dds", a, 256, 256, 8));  // canonical duplicate
	REQUIRE_FALSE(sources.Observe("textures/a.dds", b, 256, 256, 8));
	REQUIRE(sources.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Ambiguous);

	DX12Native::VerifiedTextureSources distinct;
	REQUIRE(distinct.Observe("textures/a.dds", a, 256, 256, 8));
	REQUIRE(distinct.Observe("textures/n.dds", a, 256, 256, 8));
	REQUIRE(distinct.Observe("textures/r.dds", b, 256, 256, 8));
	recipe.textures[static_cast<size_t>(NativePBR::TextureRole::Normal)].resource = { 128, 256, 8, 1, 0, 0, 0, 0, 1, true };
	REQUIRE(distinct.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Incompatible);
	recipe.textures[static_cast<size_t>(NativePBR::TextureRole::Normal)].resource.width = 256;
	REQUIRE(distinct.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Available);
	recipe.textures[static_cast<size_t>(NativePBR::TextureRole::Emission)].assetPath = "textures/e.dds";
	recipe.textures[static_cast<size_t>(NativePBR::TextureRole::Emission)].canonicalPath = "textures/e.dds";
	REQUIRE(distinct.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Missing);
	REQUIRE_FALSE(distinct.Observe("../outside.dds", a, 256, 256, 8));
	REQUIRE_FALSE(distinct.Observe("textures/e.dds", "bad-digest", 256, 256, 8));
}

TEST_CASE("Live emission overrides require finite nonnegative instance properties", "[native_pbr]")
{
	NativePBR::InstanceProperties properties;
	REQUIRE_FALSE(NativePBR::ValidLiveEmission(properties));
	properties.emissionKnown = true;
	properties.emissiveColor = { 0.5f, 1.0f, 0.1f };
	properties.emissiveMultiplier = 2.0f;
	REQUIRE(NativePBR::ValidLiveEmission(properties));
	properties.emissiveColor[0] = -0.1f;
	REQUIRE_FALSE(NativePBR::ValidLiveEmission(properties));
	properties.emissiveColor[0] = 0.5f;
	properties.emissiveMultiplier = std::numeric_limits<float>::infinity();
	REQUIRE_FALSE(NativePBR::ValidLiveEmission(properties));
}

TEST_CASE("Loose PBR texture planner is bounded, deduplicated and excludes prepared sources", "[native_pbr]")
{
	NativePBR::Recipe recipe;
	recipe.identity = recipe.revision = 1;
	recipe.parameters.texturePathsComplete = true;
	recipe.parameters.texturePaths = { "textures/base.dds", "textures/normal.dds",
		"textures/rmaos.dds", "textures/glow.dds", "", "", "" };
	recipe.textures[0].canonicalPath = "textures/base.dds";
	recipe.textures[1].canonicalPath = "textures/normal.dds";
	recipe.textures[2].canonicalPath = "textures/rmaos.dds";
	recipe.textures[3].canonicalPath = "textures/glow.dds";
	const std::shared_ptr<const NativePBR::Recipe> owned = std::make_shared<const NativePBR::Recipe>(recipe);
	NativePBR::LiveMatchResult matches;
	matches.matches.emplace_back(NativePBR::VerifiedLiveMatch{ owned, {} });
	matches.matches.emplace_back(NativePBR::VerifiedLiveMatch{ owned, {} });
	auto plan = DX12Native::PlanLiveLooseTextures(matches, { "textures/base.dds" }, 2);
	REQUIRE(plan.paths.size() == 2);
	REQUIRE(plan.paths[0] == "textures/normal.dds");
	REQUIRE(plan.paths[1] == "textures/rmaos.dds");
	REQUIRE(plan.alreadyPresent == 1);
	REQUIRE(plan.deferred == 1);
	plan = DX12Native::PlanLiveLooseTextures(matches, {}, 8);
	REQUIRE(plan.paths.size() == 4);
	REQUIRE(plan.deferred == 0);
	recipe.textures[3].canonicalPath = "textures/glow.png";
	matches.matches.clear();
	matches.matches.emplace_back(NativePBR::VerifiedLiveMatch{
		std::make_shared<const NativePBR::Recipe>(recipe), {} });
	plan = DX12Native::PlanLiveLooseTextures(matches, {}, 8);
	REQUIRE(plan.paths.size() == 3);
	REQUIRE(plan.invalid == 1);
}

TEST_CASE("Native GPU source preparation includes feature DDS without changing legacy source requests", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 9, 2, {} };
	snapshot.material.texturePathsComplete = true;
	snapshot.material.texturePaths = { "textures/c.dds", "textures/n.dds", "textures/r.dds",
		"textures/e.dds", "textures/d.dds", "textures/f0.dds", "textures/f1.dds" };
	const auto recipe = std::make_shared<const NativePBR::Recipe>(NativePBR::CompileRecipe(snapshot));
	REQUIRE(recipe->issues & NativePBR::UnsupportedDisplacement);
	REQUIRE(recipe->issues & NativePBR::UnsupportedFeatureTextures);
	NativePBR::LiveMatchResult matches;
	matches.matches.push_back(NativePBR::VerifiedLiveMatch{ recipe, {} });
	const auto legacy = DX12Native::PlanLiveLooseTextures(matches, {}, 8);
	REQUIRE(legacy.paths.size() == 4);
	const auto native = DX12Native::PlanLiveLooseTextures(matches, {}, 8, {}, true);
	REQUIRE(native.paths.size() == 7);
	REQUIRE(native.paths[4] == "textures/d.dds");
	REQUIRE(native.paths[5] == "textures/f0.dds");
	REQUIRE(native.paths[6] == "textures/f1.dds");
	REQUIRE(native.featureLimitedRecipes == 1);
}

TEST_CASE("Native material path lookup resolves slash- and case-normalized loose DDS aliases", "[native_pbr]")
{
	REQUIRE(DX12Native::MaterialTextureLookupKey("Textures\\MyMod\\RMAOS.DDS") ==
			"textures\\mymod\\rmaos.dds");
	REQUIRE(DX12Native::MaterialTextureLookupKey("textures/mymod/rmaos.dds") ==
			"textures\\mymod\\rmaos.dds");
	REQUIRE(DX12Native::MaterialTextureLookupKey("MyMod/RMAOS.dds") ==
			"textures\\mymod\\rmaos.dds");
	REQUIRE(DX12Native::MaterialTextureLookupKey("") == "");
}

TEST_CASE("Archive DDS requests accept only canonical bounded texture paths", "[native_pbr]")
{
	REQUIRE(DX12Native::ValidArchiveDdsPath("textures/mymod/color.dds"));
	REQUIRE_FALSE(DX12Native::ValidArchiveDdsPath("textures/mymod/color.DDS"));
	REQUIRE_FALSE(DX12Native::ValidArchiveDdsPath("textures/../outside.dds"));
	REQUIRE_FALSE(DX12Native::ValidArchiveDdsPath("../outside.dds"));
	REQUIRE_FALSE(DX12Native::ValidArchiveDdsPath("C:/Data/textures/a.dds"));
	REQUIRE_FALSE(DX12Native::ValidArchiveDdsPath("textures/mymod/a.png"));
	REQUIRE_FALSE(DX12Native::ValidArchiveDdsPath("textures/mymod//a.dds"));
	REQUIRE(DX12Native::kMaximumArchiveDdsBytes == 32ull * 1024 * 1024);
}

TEST_CASE("Live DX11 texture route assessment never treats descriptor hints as transferable handles", "[native_pbr]")
{
	NativePBR::TextureResourceInfo info{};
	REQUIRE(DX12Native::ClassifyLiveGpuTexture(info) == DX12Native::LiveGpuTextureRoute::Unobserved);
	info = { 1024, 512, 10, 1, uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM),
		0, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 1, true };
	REQUIRE(DX12Native::ClassifyLiveGpuTexture(info) == DX12Native::LiveGpuTextureRoute::CopyCandidate);
	info.miscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	REQUIRE(DX12Native::ClassifyLiveGpuTexture(info) == DX12Native::LiveGpuTextureRoute::PotentialSharedHandle);
	info.sampleCount = 4;
	REQUIRE(DX12Native::ClassifyLiveGpuTexture(info) == DX12Native::LiveGpuTextureRoute::UnsupportedShape);
	info.sampleCount = 1;
	info.arraySize = 6;
	REQUIRE(DX12Native::ClassifyLiveGpuTexture(info) == DX12Native::LiveGpuTextureRoute::UnsupportedShape);

	NativePBR::Snapshot snapshot{ 1, 1, {} };
	snapshot.material.texturePaths = { "textures/a.dds", "textures/n.dds", "textures/r.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	auto recipe = NativePBR::CompileRecipe(snapshot);
	auto report = DX12Native::AssessLiveGpuRecipe(recipe);
	REQUIRE(report.unobserved == 3);
	REQUIRE(report.copyCandidates == 0);
	info.arraySize = 1;
	info.miscFlags = 0;
	recipe.textures[0].resource = info;
	report = DX12Native::AssessLiveGpuRecipe(recipe);
	REQUIRE(report.unobserved == 2);
	REQUIRE(report.copyCandidates == 1);
	REQUIRE(report.unsupported == 0);
}

TEST_CASE("Runtime-authoritative matching ignores inaccurate offline material properties", "[native_pbr]")
{
	NativePBR::Registry registry;
	int runtimeMaterial = 0;
	NativePBR::MaterialData data{};
	data.texturePaths = { "textures/live/color.dds", "textures/live/normal.dds",
		"textures/live/packed.dds", "", "", "", "" };
	data.texturePathsComplete = true;
	auto token = registry.BeginCapture(&runtimeMaterial);
	REQUIRE(registry.Publish(&runtimeMaterial, token, data));
	auto published = std::make_shared<NativePBR::RecipeGeneration>();
	published->revision = registry.Revision();
	published->batchesComplete = true;
	published->recipes.emplace(token.identity, std::make_shared<const NativePBR::Recipe>(
												   NativePBR::CompileRecipe({ token.identity, 1, data })));
	NativePBR::PreparedMatchInput prepared;
	prepared.referenceKey = "A:Skyrim.esm";
	prepared.shapeName = "Cube";
	prepared.opaque = true;
	prepared.texturePaths = { "textures/offline/incorrect.dds", "", "", "" };
	prepared.alpha = 0.1f;
	prepared.pbr = false;
	prepared.valid = false;
	NativePBR::InstanceHint hint;
	hint.materialIdentity = token.identity;
	hint.referenceKey = prepared.referenceKey;
	hint.shapeName = prepared.shapeName;
	NativePBR::LiveBindingSource source{ published, { hint } };
	const auto matches = NativePBR::ResolveRuntimeMatches({ prepared }, source);
	REQUIRE(matches.report.matched == 1);
	REQUIRE(matches.matches[0]);
	REQUIRE(matches.matches[0]->recipe->parameters.texturePaths[0] == "textures/live/color.dds");
	REQUIRE(NativePBR::ResolveRuntimeMatches({ prepared, prepared }, source).report.ambiguous == 2);
	source.hints.push_back(hint);
	REQUIRE(NativePBR::ResolveRuntimeMatches({ prepared }, source).report.ambiguous == 1);
	source.hints.pop_back();
	source.hints[0].properties.alphaProperty = true;
	REQUIRE(NativePBR::ResolveRuntimeMatches({ prepared }, source).report.incompatible == 1);
	source.hints[0].properties.alphaProperty = false;
	prepared.opaque = false;
	REQUIRE(NativePBR::ResolveRuntimeMatches({ prepared }, source).report.incompatible == 1);
}

TEST_CASE("Runtime PBR material compiler takes texture bindings and parameters from live recipe", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 5, 7, {} };
	snapshot.material.texturePaths = { "textures/live/c.dds", "textures/live/n.dds",
		"textures/live/r.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	snapshot.material.roughnessScale = 0.6f;
	snapshot.material.specularLevel = 0.12f;
	snapshot.material.uvOffsetScale[0] = { 0.2f, 0.3f, 1.5f, 2.0f };
	const auto recipe = std::make_shared<const NativePBR::Recipe>(NativePBR::CompileRecipe(snapshot));
	NativePBR::InstanceProperties properties;
	properties.shaderFlags = 1ull << 37;  // runtime vertex colors
	properties.emissionKnown = true;
	properties.emissiveColor = { 0.25f, 0.5f, 0.75f };
	properties.emissiveMultiplier = 3.0f;
	NativePBR::VerifiedLiveMatch match{ recipe, properties };
	DX12Native::MaterialResources resources;
	for (size_t i = 0; i < 3; ++i) {
		const auto path = DX12Native::MaterialTextureLookupKey(recipe->textures[i].canonicalPath);
		resources.locations.emplace(path, i + 1);
		resources.logicalLocations.emplace(i + 1, std::array<uint32_t, 2>{ 0u, uint32_t(i) });
	}
	DX12Native::Material source;
	auto attributes = std::make_shared<DX12Native::VertexAttributes>();
	attributes->hasTangentFrame = true;
	source.attributes = attributes;
	// Inaccurate / missing offline metadata cannot influence the runtime compiler.
	const auto result = DX12Native::CompileRuntimeMaterial(resources, source, match);
	REQUIRE(result.status == DX12Native::RuntimeMaterialStatus::Ready);
	const std::array<uint32_t, 4> expectedColorNormal{ 0, 0, 0, 1 };
	REQUIRE(result.record.colorNormal == expectedColorNormal);
	REQUIRE(result.record.packedEmission[0] == 0);
	REQUIRE(result.record.packedEmission[1] == 2);
	REQUIRE(result.record.scalar.z == 0.12f);
	REQUIRE(result.record.scalar.w == 0.6f);
	REQUIRE(result.record.uv.x == 1.5f);
	REQUIRE(result.record.uv.z == 0.2f);
	REQUIRE(result.record.flags[0] & 8);
	REQUIRE(result.emissionApplied);
	REQUIRE(result.record.emission.w == 3.0f);
	auto missing = resources;
	missing.locations.erase(DX12Native::MaterialTextureLookupKey("textures/live/r.dds"));
	REQUIRE(DX12Native::CompileRuntimeMaterial(missing, source, match).status ==
			DX12Native::RuntimeMaterialStatus::MissingTextures);
	match.properties.shaderFlags |= 1ull << 3;
	REQUIRE(DX12Native::CompileRuntimeMaterial(resources, source, match).status ==
			DX12Native::RuntimeMaterialStatus::UnsafeOpacity);
}

TEST_CASE("Runtime PBR issue instrumentation counts overlapping causes without baseline unknowns", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 10, 11, {} };
	snapshot.material.texturePaths = { "textures/base.dds", "textures/n.dds", "textures/r.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	snapshot.material.pbrFlags = (1u << 0) | (1u << 5);  // subsurface and fuzz
	snapshot.material.glint.enabled = true;
	const auto recipe = NativePBR::CompileRecipe(snapshot);
	NativePBR::RecipeIssueTally counts;
	counts.Observe(recipe);
	REQUIRE(counts.affected == 1);
	REQUIRE(counts.issues[2] == 1);
	REQUIRE(counts.issues[3] == 1);
	REQUIRE(counts.issues[4] == 0);  // UnknownOpacity is a baseline hint
	REQUIRE(counts.issues[9] == 0);
	REQUIRE(counts.pbrFlags[0] == 1);
	REQUIRE(counts.pbrFlags[5] == 1);
	counts.Observe(recipe);
	REQUIRE(counts.affected == 2);
	REQUIRE(counts.issues[2] == 2);
}

TEST_CASE("Missing source planner prioritizes native-ready recipes then feature-limited meshes", "[native_pbr]")
{
	NativePBR::Snapshot readySnapshot{ 101, 2, {} };
	readySnapshot.material.texturePaths = { "textures/ready/c.dds", "textures/ready/n.dds",
		"textures/ready/r.dds", "", "", "", "" };
	readySnapshot.material.texturePathsComplete = true;
	NativePBR::Snapshot limitedSnapshot{ 102, 3, {} };
	limitedSnapshot.material.texturePaths = { "textures/limited/c.dds", "textures/limited/n.dds",
		"textures/limited/r.dds", "", "", "", "" };
	limitedSnapshot.material.texturePathsComplete = true;
	limitedSnapshot.material.pbrFlags = 1u << 5;  // fuzz requires a dedicated shading implementation
	auto ready = std::make_shared<const NativePBR::Recipe>(NativePBR::CompileRecipe(readySnapshot));
	auto limited = std::make_shared<const NativePBR::Recipe>(NativePBR::CompileRecipe(limitedSnapshot));
	REQUIRE_FALSE(NativePBR::NativeMeshRecipeSupported(*limited));
	NativePBR::LiveMatchResult matches;
	matches.matches.push_back(NativePBR::VerifiedLiveMatch{ limited, {} });
	matches.matches.push_back(NativePBR::VerifiedLiveMatch{ ready, {} });
	matches.matches.push_back(NativePBR::VerifiedLiveMatch{ limited, {} });
	const auto planned = DX12Native::PlanLiveLooseTextures(matches, {}, 4);
	REQUIRE(planned.paths.size() == 4);
	REQUIRE(planned.paths[0] == "textures/ready/c.dds");
	REQUIRE(planned.paths[1] == "textures/ready/n.dds");
	REQUIRE(planned.paths[2] == "textures/ready/r.dds");
	REQUIRE(planned.paths[3] == "textures/limited/c.dds");
	REQUIRE(planned.deferred == 2);
	REQUIRE(planned.supportedRecipes == 1);
	REQUIRE(planned.featureLimitedRecipes == 1);
	REQUIRE(planned.skippedRecipes == 0);
	const auto notImported = DX12Native::PlanLiveLooseTextures(matches,
		{ "textures/ready/c.dds", "textures/ready/n.dds", "textures/ready/r.dds" }, 4);
	REQUIRE(notImported.paths[0] == "textures/limited/c.dds");
	REQUIRE(notImported.alreadyPresent == 3);
}

TEST_CASE("Native runtime PBR accepts only provably neutral fuzz and subsurface flags", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 510, 5, {} };
	snapshot.material.texturePaths = { "textures/c.dds", "textures/n.dds", "textures/r.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	snapshot.material.pbrFlags = 1u << 5;
	snapshot.material.fuzzWeight = 0.0f;
	auto recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE((recipe.issues & NativePBR::UnsupportedPbrFeatures) != 0);
	REQUIRE((NativePBR::EffectiveNativeIssues(recipe) & NativePBR::UnsupportedPbrFeatures) == 0);
	REQUIRE(NativePBR::NativeMeshRecipeSupported(recipe));
	snapshot.material.fuzzWeight = 0.25f;
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE_FALSE(NativePBR::NativeMeshRecipeSupported(recipe));

	snapshot.material.pbrFlags = 1u << 0;
	snapshot.material.subsurfaceOpacity = 1.0f;
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(NativePBR::NativeMeshRecipeSupported(recipe));
	snapshot.material.subsurfaceOpacity = 0.5f;
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE_FALSE(NativePBR::NativeMeshRecipeSupported(recipe));

	snapshot.material.pbrFlags = (1u << 0) | (1u << 5);
	snapshot.material.subsurfaceOpacity = 1.0f;
	snapshot.material.fuzzWeight = 0.0f;
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(NativePBR::NativeMeshRecipeSupported(recipe));
	snapshot.material.pbrFlags |= 1u << 1;  // active coat: still unsupported
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE_FALSE(NativePBR::NativeMeshRecipeSupported(recipe));
	snapshot.material.pbrFlags = 1u << 5;
	snapshot.material.fuzzWeight = 0.0f;
	snapshot.material.texturePaths[6] = "textures/feature.dds";
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE_FALSE(NativePBR::NativeMeshRecipeSupported(recipe));
}

TEST_CASE("Native material source supports future manifest-free runtime acquisition", "[native_pbr]")
{
	DX12Native::MaterialSource source;
	REQUIRE(source.usePreparedTextureManifest);
	REQUIRE_FALSE(source.importLoosePbr);
	source.usePreparedTextureManifest = false;
	source.importLoosePbr = true;
	REQUIRE_FALSE(source.usePreparedTextureManifest);
	// The source loader can omit cell-textures.json when runtime sources
	// and a verified live match plan are supplied; prepared geometry is separate.
	NativePBR::Snapshot snapshot{ 811, 1, {} };
	snapshot.material.texturePaths = { "textures/runtime/c.dds", "textures/runtime/n.dds",
		"textures/runtime/r.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	auto recipe = std::make_shared<const NativePBR::Recipe>(NativePBR::CompileRecipe(snapshot));
	NativePBR::LiveMatchResult matches;
	matches.matches.push_back(NativePBR::VerifiedLiveMatch{ recipe, {} });
	const auto requests = DX12Native::PlanLiveLooseTextures(matches, {});
	REQUIRE(requests.paths.size() == 3);
	REQUIRE(requests.paths[0] == "textures/runtime/c.dds");
}

TEST_CASE("Progressive source request planner does not retry failed batches", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 900, 1, {} };
	snapshot.material.texturePaths = { "textures/a.dds", "textures/b.dds", "textures/c.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	auto recipe = std::make_shared<const NativePBR::Recipe>(NativePBR::CompileRecipe(snapshot));
	NativePBR::LiveMatchResult matches;
	matches.matches.push_back(NativePBR::VerifiedLiveMatch{ recipe, {} });
	DX12Native::RuntimeTextureProgress progress;
	const auto first = DX12Native::PlanLiveLooseTextures(matches, {}, 2, progress.Attempted());
	REQUIRE(first.paths.size() == 2);
	REQUIRE(first.deferred == 1);
	progress.Record(first.paths, { "textures/a.dds" });
	REQUIRE(progress.Batches() == 1);
	REQUIRE(progress.Successful() == 1);
	REQUIRE(progress.Failed() == 1);
	const auto next = DX12Native::PlanLiveLooseTextures(matches, { "textures/a.dds" }, 2, progress.Attempted());
	REQUIRE(next.paths.size() == 1);
	REQUIRE(next.paths[0] == "textures/c.dds");
	REQUIRE(next.alreadyAttempted == 1);
	REQUIRE(next.alreadyPresent == 1);
	REQUIRE(next.deferred == 0);
	progress.Record(next.paths, {});
	REQUIRE(progress.AttemptedCount() == 3);
	REQUIRE(progress.Failed() == 2);
	const auto final = DX12Native::PlanLiveLooseTextures(matches,
		{ "textures/a.dds" }, 2, progress.Attempted());
	REQUIRE(final.paths.empty());
	REQUIRE(final.deferred == 0);
	progress.Reset();
	REQUIRE(progress.AttemptedCount() == 0);
	REQUIRE(progress.Batches() == 0);
}

TEST_CASE("Runtime-only DDS acquisition bypasses legacy manifest without changing geometry", "[native_pbr]")
{
	REQUIRE(DX12Native::UsePreparedMaterialManifest(false, false));
	REQUIRE_FALSE(DX12Native::UsePreparedMaterialManifest(true, false));
	REQUIRE_FALSE(DX12Native::UsePreparedMaterialManifest(false, true));
	REQUIRE_FALSE(DX12Native::UsePreparedMaterialManifest(true, true));
	DX12Native::MaterialSource source;
	REQUIRE(source.usePreparedTextureManifest);
	source.usePreparedTextureManifest = DX12Native::UsePreparedMaterialManifest(true, false);
	source.importLoosePbr = true;
	REQUIRE_FALSE(source.usePreparedTextureManifest);
	REQUIRE(source.importLoosePbr);
}

TEST_CASE("Live geometry capture owns bounded static mesh data and rejects unsafe layouts", "[native_geometry]")
{
	std::array<float, 12> world{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
	std::vector<uint8_t> raw(3 * 16);
	const std::array<std::array<float, 4>, 3> positions{ { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 } } };
	std::memcpy(raw.data(), positions.data(), raw.size());
	const std::array<uint16_t, 3> indices{ 0, 1, 2 };
	DX12Native::RuntimeGeometryInput input;
	input.referenceKey = "000001:Skyrim.esm";
	input.shapeName = "Wall";
	input.transform = world;
	input.bound = { 0, 0, 0, 2 };
	input.vertexCount = 3;
	input.vertexStride = 16;
	input.triangleCount = 1;
	input.vertices = raw;
	input.indices = indices;
	REQUIRE(DX12Native::ValidRuntimeGeometryInput(input));
	auto packet = DX12Native::CopyRuntimeGeometry(input);
	REQUIRE(packet);
	REQUIRE(packet->indexCount == 3);
	DX12Native::RuntimeGeometryMailbox mailbox(100, 1);
	REQUIRE(mailbox.Submit(packet));
	REQUIRE(mailbox.Contains(input.referenceKey, input.shapeName));
	REQUIRE(mailbox.Count() == 1);
	raw[0] = 99;
	REQUIRE(packet->vertices[0] != raw[0]);  // no borrowed engine bytes
	REQUIRE_FALSE(mailbox.Submit(packet));
	REQUIRE(mailbox.Export(0, 1).size() == 1);
	REQUIRE(mailbox.Export(1, 1).empty());
	input.skinned = true;
	REQUIRE_FALSE(DX12Native::CopyRuntimeGeometry(input));
	input.skinned = false;
	input.dynamic = true;
	REQUIRE_FALSE(DX12Native::ValidRuntimeGeometryInput(input));
	input.dynamic = false;
	input.triangleCount = 0;
	REQUIRE_FALSE(DX12Native::ValidRuntimeGeometryInput(input));
	input.triangleCount = 1;
	mailbox.Reset();
	REQUIRE(mailbox.Count() == 0);
}

TEST_CASE("Runtime geometry encoder creates native index/position payload without atlas files", "[native_geometry]")
{
	DX12Native::RuntimeGeometryPacket packet;
	packet.referenceKey = "A:Skyrim.esm";
	packet.shapeName = "Cube";
	packet.vertexCount = 3;
	packet.vertexStride = 16;
	packet.indexCount = 3;
	packet.indices = { 0, 1, 2 };
	packet.vertices.resize(48);
	const std::array<std::array<float, 4>, 3> xyz{ { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 } } };
	std::memcpy(packet.vertices.data(), xyz.data(), packet.vertices.size());
	auto encoded = DX12Native::EncodeRuntimeGeometry(packet);
	REQUIRE(encoded);
	REQUIRE(encoded->geometry.size() == 16 + 3 * 12 + 3 * 4);
	REQUIRE(encoded->attributes.empty());
	uint32_t magic = 0;
	std::memcpy(&magic, encoded->geometry.data(), 4);
	REQUIRE(magic == 0x4d475352u);
	packet.indices[1] = 42;
	REQUIRE_FALSE(DX12Native::EncodeRuntimeGeometry(packet));
	packet.indices[1] = 1;
	packet.vertices[0] = 0xFF;
	packet.vertices[1] = 0xFF;
	packet.vertices[2] = 0xFF;
	packet.vertices[3] = 0x7F;  // non-finite position
	REQUIRE_FALSE(DX12Native::EncodeRuntimeGeometry(packet));
	REQUIRE(DX12Native::GeometryHalfToFloat(0x3c00) == 1.0f);
}

TEST_CASE("Content addressed native geometry cache roundtrips only verified static source data", "[native_geometry]")
{
	DX12Native::RuntimeGeometryPacket packet;
	packet.referenceKey = "000001:Skyrim.esm";
	packet.shapeName = "Wall";
	packet.transform = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
	packet.bound = { 0, 0, 0, 2 };
	packet.vertexCount = 3;
	packet.vertexStride = 16;
	packet.indexCount = 3;
	packet.indices = { 0, 1, 2 };
	packet.vertices.resize(48);
	const std::array<std::array<float, 4>, 3> positions{ { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 } } };
	std::memcpy(packet.vertices.data(), positions.data(), packet.vertices.size());
	const auto encoded = DX12Native::SerializeRuntimeGeometryCache(packet);
	REQUIRE(encoded);
	REQUIRE(encoded->size() > 128);
	const auto digest = DX12Native::RuntimeGeometryContentId(packet);
	REQUIRE(digest);
	auto restored = DX12Native::DeserializeRuntimeGeometryCache(*encoded);
	REQUIRE(restored);
	REQUIRE(restored->referenceKey == packet.referenceKey);
	REQUIRE(restored->shapeName == packet.shapeName);
	REQUIRE(restored->vertices == packet.vertices);
	REQUIRE(restored->indices == packet.indices);
	REQUIRE(DX12Native::RuntimeGeometryContentId(*restored) == digest);
	// A cached reference alone is insufficient: a changed world transform
	// must produce a different digest even if shape and vertex bytes match.
	auto moved = *restored;
	moved.transform[3] += 10.0f;
	REQUIRE(DX12Native::RuntimeGeometryContentId(moved) != digest);
	auto retopologized = *restored;
	retopologized.indices = { 0, 2, 1 };
	REQUIRE(DX12Native::RuntimeGeometryContentId(retopologized) != digest);
	auto corrupt = *encoded;
	corrupt[0] ^= 0x1;
	REQUIRE_FALSE(DX12Native::DeserializeRuntimeGeometryCache(corrupt));
	corrupt = *encoded;
	corrupt.pop_back();
	REQUIRE_FALSE(DX12Native::DeserializeRuntimeGeometryCache(corrupt));
	packet.indices[2] = 300;
	REQUIRE_FALSE(DX12Native::SerializeRuntimeGeometryCache(packet));
}

TEST_CASE("Geometry cache files have a distinct extension and independently verified contents", "[native_geometry]")
{
	const auto root = std::filesystem::current_path() /
	                  ("native-geometry-test-" + std::to_string(GetCurrentProcessId()));
	struct Cleanup
	{
		std::filesystem::path folder;
		~Cleanup()
		{
			std::error_code ec;
			std::filesystem::remove_all(folder, ec);
		}
	} cleanup{ root };
	DX12Native::MaterialDiskCache cache(root, 2 * 1024 * 1024, ".rgc");
	std::vector<uint8_t> bytes(192, 42);
	auto hash = Util::FileDigest::Sha256BytesHex(bytes);
	REQUIRE(hash);
	REQUIRE(cache.Store(*hash, bytes) == DX12Native::MaterialDiskCache::StoreResult::Stored);
	REQUIRE(cache.ListDigests(1) == std::vector<std::string>{ *hash });
	REQUIRE(cache.Read(*hash, 1024) == bytes);
	bytes[0] ^= 1;
	REQUIRE(cache.Store(*hash, bytes) == DX12Native::MaterialDiskCache::StoreResult::Unavailable);
	std::ofstream modify(root / (*hash + ".rgc"), std::ios::binary | std::ios::trunc);
	modify.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	modify.close();
	REQUIRE_FALSE(cache.Read(*hash, 1024));
}

TEST_CASE("Native PBR interprets JSON and NIF data without an engine feature object", "[native_pbr]")
{
	NativePBR::MaterialData base;
	base.pbrFlags = uint32_t(PBRFlags::Subsurface) | uint32_t(PBRFlags::Fuzz);
	base.nifThinSurfaceFlags = uint32_t(PBRFlags::ThinSurface);
	PBRRecords::TextureSetData config;
	config.roughnessScale = 0.7f;
	config.subsurfaceOpacity = 0.6f;
	config.subsurfaceBalance = 0.8f;
	config.fuzzWeight = 0.25f;
	config.fuzzRoughness = 0.3f;
	config.glintParameters.enabled = true;
	config.thinSurface = false;
	const auto interpreted = NativePBR::InterpretTextureSet(base, config,
		std::array{ 0.2f, 0.3f, 0.4f, 2.0f }, std::array{ 0.8f, 1.0f });
	REQUIRE(interpreted.roughnessScale == 0.7f);
	REQUIRE(interpreted.subsurfaceOpacity == 0.6f);
	REQUIRE(interpreted.subsurfaceBalance == 0.2f);
	REQUIRE(interpreted.fuzzWeight == 0.25f);
	REQUIRE(interpreted.fuzzRoughness == 0.8f);
	REQUIRE(interpreted.fuzzRoughnessTexture);
	REQUIRE(interpreted.glint.enabled);
	REQUIRE(interpreted.pbrFlags & uint32_t(PBRFlags::ThinSurface));
	REQUIRE(base.roughnessScale == 1.0f);
	REQUIRE(base.fuzzRoughness == -1.0f);
	auto invalid = interpreted;
	const std::array badNif{ 0.5f, std::numeric_limits<float>::quiet_NaN() };
	REQUIRE_FALSE(NativePBR::InterpretNifFuzz(badNif, invalid));
	REQUIRE(invalid == interpreted);
}

TEST_CASE("Native PBR material service keeps MATO reference-local across generations", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	NativePBR::MaterialService service;
	int source = 0;
	NativePBR::MaterialData data;
	data.roughnessScale = 0.55f;
	const auto capture = registry.BeginCapture(&source);
	REQUIRE(registry.Publish(&source, capture, data));
	const auto changes = *registry.ExportChanges(0, {});
	cache.Update(changes);
	const auto recipes = NativePBR::PublishRecipes(cache, changes, {});
	auto catalog = std::make_shared<NativePBR::MaterialCatalogSnapshot>();
	PBRRecords::MaterialObjectData red;
	red.baseColorScale = { 1.0f, 0.2f, 0.3f };
	red.roughness = 0.25f;
	red.glintParameters.enabled = true;
	PBRRecords::MaterialObjectData blue;
	blue.baseColorScale = { 0.2f, 0.2f, 1.0f };
	catalog->materialObjects.emplace("redmato", red);
	catalog->materialObjects.emplace("bluemato", blue);
	NativePBR::InstanceProperties first;
	first.materialObjectEditorId = "RedMato";
	first.opacity = NativePBR::InstanceProperties::Opacity::NonOpaque;
	first.normalMode = NativePBR::InstanceProperties::NormalMode::TangentSpace;
	NativePBR::InstanceProperties second;
	second.materialObjectEditorId = "BlueMato";
	std::vector<NativePBR::InstanceHint> hints{ { capture.identity, "ref-a", "Body", first },
		{ capture.identity, "ref-b", "Body", second } };
	REQUIRE(service.Publish(recipes, catalog, hints));
	const auto generation = service.GetSnapshot();
	REQUIRE(generation);
	const auto shared = generation->FindMaterial(capture.identity, recipes->recipes.at(capture.identity)->revision);
	REQUIRE(shared);
	REQUIRE(shared.recipe->parameters.roughnessScale == 0.55f);
	REQUIRE(shared.recipe->parameters.projectedRoughness == 1.0f);
	REQUIRE_FALSE(shared.instanceOverride);
	const auto a = generation->FindInstance("REF-A", "body");
	const auto b = generation->FindInstance("ref-b", "BODY");
	REQUIRE(a);
	REQUIRE(b);
	REQUIRE(a.recipe == b.recipe);
	REQUIRE(a.instanceOverride->roughness == 0.25f);
	REQUIRE(b.instanceOverride->roughness == 1.0f);
	REQUIRE(a.instanceIssues & NativePBR::UnsupportedMatoGlint);
	REQUIRE_FALSE(b.instanceIssues & NativePBR::UnsupportedMatoGlint);
	REQUIRE(a.recipeIssues & NativePBR::UnsupportedProjection);
	REQUIRE_FALSE(a.recipeIssues & NativePBR::UnmatchedSceneInstance);
	REQUIRE_FALSE(a.recipeIssues & NativePBR::UnknownOpacity);
	REQUIRE_FALSE(a.recipeIssues & NativePBR::UnknownNormalMode);
	REQUIRE(b.recipeIssues & NativePBR::UnknownOpacity);
	REQUIRE(generation->FindMaterial(capture.identity, 999).status == NativePBR::MaterialLookupStatus::RevisionMismatch);
	REQUIRE(generation->FindInstance("missing", "Body").status == NativePBR::MaterialLookupStatus::MissingInstance);

	REQUIRE(service.Publish(recipes, catalog, {}));
	REQUIRE(service.GetSnapshot()->FindInstance("ref-a", "body").status == NativePBR::MaterialLookupStatus::MissingInstance);
	REQUIRE(a.instanceOverride->roughness == 0.25f);
	REQUIRE(a.recipe->parameters.roughnessScale == 0.55f);
	REQUIRE(generation->FindInstance("ref-b", "Body"));
}

TEST_CASE("Native PBR publication reports missing MATO, ambiguity and retired materials", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	NativePBR::MaterialService service;
	int source = 0;
	const auto token = registry.BeginCapture(&source);
	REQUIRE(registry.Publish(&source, token, {}));
	auto changes = *registry.ExportChanges(0, {});
	cache.Update(changes);
	auto recipes = NativePBR::PublishRecipes(cache, changes, {});
	NativePBR::InstanceProperties properties;
	properties.materialObjectEditorId = "UnknownMato";
	std::vector<NativePBR::InstanceHint> hints{ { token.identity, "ref", "shape", properties } };
	REQUIRE(service.Publish(recipes, {}, hints));
	auto publication = service.GetSnapshot();
	const auto missingMato = publication->FindInstance("ref", "shape");
	REQUIRE(missingMato);
	REQUIRE(missingMato.instanceIssues & NativePBR::MissingMatoConfiguration);
	REQUIRE(missingMato.missingMatoEditorId == "UnknownMato");
	REQUIRE(missingMato.recipeIssues & NativePBR::MissingRequiredTextures);
	hints.push_back({ token.identity + 1, "ref", "shape" });
	REQUIRE(service.Publish(recipes, {}, hints));
	REQUIRE(service.GetSnapshot()->FindInstance("ref", "shape").status == NativePBR::MaterialLookupStatus::AmbiguousInstance);
	registry.MarkDirty(&source);
	changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	cache.Update(changes);
	const auto pending = NativePBR::PublishRecipes(cache, changes, recipes);
	REQUIRE(pending->pendingIdentities.contains(token.identity));
	REQUIRE(service.Publish(pending, {}, hints));
	REQUIRE(service.GetSnapshot()->FindMaterial(token.identity).status == NativePBR::MaterialLookupStatus::PendingMaterial);
	registry.Forget(&source);
	changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	cache.Update(changes);
	const auto retired = NativePBR::PublishRecipes(cache, changes, pending);
	REQUIRE(service.Publish(retired, {}, {}));
	REQUIRE(service.GetSnapshot()->FindMaterial(token.identity).status == NativePBR::MaterialLookupStatus::MissingMaterial);
	REQUIRE(publication->FindMaterial(token.identity));
	REQUIRE_FALSE(service.Publish(recipes, {}, {}));
}

TEST_CASE("Native material discovery owns identities, NIF interpretation and JSON reapplication", "[native_pbr]")
{
	NativePBR::MaterialDiscovery discovery;
	auto catalog = std::make_shared<NativePBR::MaterialCatalogSnapshot>();
	PBRRecords::TextureSetData config;
	config.roughnessScale = 0.4f;
	config.fuzzWeight = 0.3f;
	catalog->textureSets.emplace("mytexture", config);
	NativePBR::MaterialSource source;
	source.base.texturePaths = { "textures/test/color.dds", "textures/test/normal.dds", "textures/test/rmaos.dds", "", "", "", "" };
	source.base.texturePathsComplete = true;
	source.textureSetEditorId = "MyTexture";
	source.nifFlags = PBRRecords::NifSurfaceFlags{ .softLighting = true };
	source.nifFuzz = std::array{ 0.8f, 1.0f };
	const auto first = discovery.Register("Meshes/Example.nif|Shape", source, catalog);
	REQUIRE(first.status == NativePBR::MaterialSourceReport::Status::Published);
	REQUIRE(first.identity > 0);
	REQUIRE(first.issues == 0);
	REQUIRE(discovery.Count() == 1);
	REQUIRE(discovery.Inspect(first.identity).revision == first.revision);
	REQUIRE(discovery.Register("meshes/example.nif|shape", source, catalog).status == NativePBR::MaterialSourceReport::Status::Unchanged);
	REQUIRE(discovery.GetRegistry().Revision() == first.revision);
	const auto published = discovery.GetRegistry().Export();
	REQUIRE(published.size() == 1);
	REQUIRE(published[0].material.roughnessScale == 0.4f);
	REQUIRE(published[0].material.fuzzRoughness == 0.8f);
	REQUIRE(published[0].material.pbrFlags & uint32_t(PBRFlags::Fuzz));
	REQUIRE(published[0].identity == first.identity);
	const auto original = published[0];
	catalog = std::make_shared<NativePBR::MaterialCatalogSnapshot>(*catalog);
	catalog->textureSets.at("mytexture").roughnessScale = 0.9f;
	discovery.RefreshCatalog(catalog);
	const auto modified = discovery.GetRegistry().Export().front();
	REQUIRE(modified.identity == first.identity);
	REQUIRE(modified.revision > original.revision);
	REQUIRE(modified.material.roughnessScale == 0.9f);
	REQUIRE(original.material.roughnessScale == 0.4f);
	REQUIRE(discovery.Retire("Meshes/Example.nif|Shape"));
	REQUIRE(discovery.GetRegistry().Export().empty());
	REQUIRE(discovery.Inspect(first.identity).status == NativePBR::MaterialSourceReport::Status::NotFound);
	const auto recreated = discovery.Register("Meshes/Example.nif|Shape", source, catalog);
	REQUIRE(recreated.identity != first.identity);
}

TEST_CASE("Native discovery reports incomplete sources without hiding their recipes", "[native_pbr]")
{
	NativePBR::MaterialDiscovery discovery;
	NativePBR::MaterialSource source;
	source.textureSetEditorId = "MissingJson";
	source.nifThinSurface = std::array{ 0.25f, 0.5f, 0.75f, 1.0f };
	source.nifFuzz = std::array{ 0.2f, 2.0f };
	const auto published = discovery.Register("mesh|shape", source, {});
	REQUIRE(published.status == NativePBR::MaterialSourceReport::Status::Published);
	REQUIRE(published.issues & NativePBR::MissingTextureSetConfiguration);
	REQUIRE(published.issues & NativePBR::InvalidNifFuzz);
	REQUIRE(discovery.GetRegistry().Export().size() == 1);
	REQUIRE(discovery.GetRegistry().Export().front().material.subsurfaceBalance == 0.25f);
	source.base.roughnessScale = std::numeric_limits<float>::quiet_NaN();
	const auto invalid = discovery.Register("mesh|shape", source, {});
	REQUIRE(invalid.status == NativePBR::MaterialSourceReport::Status::InvalidParameters);
	REQUIRE(invalid.issues & NativePBR::NonFiniteMaterialParameters);
	REQUIRE(discovery.GetRegistry().Export().empty());
	source.base.roughnessScale = 0.7f;
	const auto restored = discovery.Register("mesh|shape", source, {});
	REQUIRE(restored.status == NativePBR::MaterialSourceReport::Status::Published);
	REQUIRE(restored.identity == published.identity);
	REQUIRE(restored.revision > published.revision);
}

TEST_CASE("Registry exports atomic material and instance epochs for safe cell transitions", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::RecipeCache cache;
	NativePBR::MaterialService service;
	int source = 0;
	const auto capture = registry.BeginCapture(&source);
	REQUIRE(registry.Publish(&source, capture, {}));
	registry.ObserveInstance(&source, "cell-a:ref", "shape");
	auto changes = *registry.ExportChanges(0, {});
	REQUIRE(changes.instanceHints.size() == 1);
	REQUIRE(changes.instanceEpoch == 0);
	cache.Update(changes);
	const auto oldRecipes = NativePBR::PublishRecipes(cache, changes, {});
	REQUIRE(service.Publish(oldRecipes, {}, changes.instanceHints, changes.instanceEpoch));
	const auto previous = service.GetSnapshot();
	REQUIRE(previous->FindInstance("cell-a:ref", "shape"));
	REQUIRE(service.GetSnapshotForRegistry(registry));
	registry.BeginInstanceEpoch();
	REQUIRE(registry.InstanceEpoch() == 1);
	REQUIRE_FALSE(service.GetSnapshotAt(registry.Revision(), registry.InstanceEpoch()));
	REQUIRE_FALSE(service.GetSnapshotForRegistry(registry));
	changes = *registry.ExportChanges(cache.Revision(), cache.KnownRevisions());
	REQUIRE(changes.instanceHints.empty());
	REQUIRE(changes.instanceEpoch == 1);
	cache.Update(changes);
	const auto nextRecipes = NativePBR::PublishRecipes(cache, changes, oldRecipes);
	REQUIRE(service.Publish(nextRecipes, {}, changes.instanceHints, changes.instanceEpoch));
	REQUIRE(service.GetSnapshotAt(registry.Revision(), registry.InstanceEpoch()));
	REQUIRE(service.GetSnapshotForRegistry(registry));
	REQUIRE(service.GetSnapshot()->FindInstance("cell-a:ref", "shape").status == NativePBR::MaterialLookupStatus::MissingInstance);
	REQUIRE(previous->FindInstance("cell-a:ref", "shape"));
	REQUIRE_FALSE(service.Publish(oldRecipes, {}, previous->sourceHints, previous->instanceEpoch));
}

TEST_CASE("Verified MATO mutations replace only the selected instance", "[native_pbr]")
{
	NativePBR::MaterialDiscovery discovery;
	NativePBR::MaterialSource source;
	const auto material = discovery.Register("model|material", source, {});
	NativePBR::InstanceProperties first;
	first.materialObjectEditorId = "MatoA";
	NativePBR::InstanceProperties second;
	second.materialObjectEditorId = "MatoB";
	REQUIRE(discovery.ObserveInstance("model|material", "ref-a", "shape", first));
	REQUIRE(discovery.ObserveInstance("model|material", "ref-b", "shape", second));
	const auto sourceRevision = discovery.GetRegistry().Export().front().revision;
	const auto previous = *discovery.GetRegistry().ExportChanges(0, {});
	first.materialObjectEditorId = "MatoC";
	REQUIRE(discovery.ReplaceInstance("model|material", "ref-a", "shape", first));
	REQUIRE_FALSE(discovery.ReplaceInstance("model|material", "ref-c", "shape", first));
	const auto changed = *discovery.GetRegistry().ExportChanges(previous.revision, {});
	REQUIRE(changed.instanceHints.size() == 2);
	REQUIRE(discovery.GetRegistry().Export().front().revision == sourceRevision);
	REQUIRE(changed.revision > previous.revision);
	const auto a = std::find_if(changed.instanceHints.begin(), changed.instanceHints.end(), [](const auto& hint) {
		return hint.referenceKey == "ref-a";
	});
	const auto b = std::find_if(changed.instanceHints.begin(), changed.instanceHints.end(), [](const auto& hint) {
		return hint.referenceKey == "ref-b";
	});
	REQUIRE(a != changed.instanceHints.end());
	REQUIRE(b != changed.instanceHints.end());
	REQUIRE(a->properties.materialObjectEditorId == "MatoC");
	REQUIRE(b->properties.materialObjectEditorId == "MatoB");
	REQUIRE_FALSE(a->propertiesConflict);
	REQUIRE(material.identity == a->materialIdentity);
}

TEST_CASE("Material readiness separates unresolved instance state from unsupported shading", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 43, 7, {} };
	snapshot.material.texturePaths = { "textures/c.dds", "textures/n.dds", "textures/r.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	auto recipe = NativePBR::CompileRecipe(snapshot);
	auto readiness = NativePBR::ClassifyRecipeReadiness(recipe);
	REQUIRE(readiness.sourceIssues == 0);
	REQUIRE(readiness.unsupportedFeatures == 0);
	REQUIRE(readiness.unresolvedInstance == NativePBR::kUnresolvedInstanceIssues);
	REQUIRE(readiness.IsOpaqueFeatureCompatible());
	const auto captured = NativePBR::ResolveCapturedOpaqueUv(readiness, true);
	REQUIRE(captured.unresolvedInstance == (NativePBR::kUnresolvedInstanceIssues & ~NativePBR::UnknownUvAnimation));
	REQUIRE(NativePBR::ResolveCapturedOpaqueUv(readiness, false).unresolvedInstance == NativePBR::kUnresolvedInstanceIssues);
	NativePBR::MaterialReadinessCounts counts;
	counts.Observe(recipe);
	REQUIRE(counts.recipes == 1);
	REQUIRE(counts.opaqueFeatureCompatible == 1);
	REQUIRE(counts.featureLimited == 0);
	REQUIRE(counts.unresolvedInstances == 1);
	snapshot.material.pbrFlags = uint32_t(PBRFlags::ColoredCoat) |
		uint32_t(PBRFlags::CoatNormal) | uint32_t(PBRFlags::InterlayerParallax);
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(NativePBR::ClassifyRecipeReadiness(recipe).unsupportedFeatures == 0);
	REQUIRE(NativePBR::NativeMeshRecipeSupported(recipe));
	snapshot.material.pbrFlags |= uint32_t(PBRFlags::TwoLayer);
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(NativePBR::ClassifyRecipeReadiness(recipe).unsupportedFeatures & NativePBR::UnsupportedPbrFeatures);
	snapshot.material.pbrFlags = uint32_t(PBRFlags::Fuzz);
	snapshot.material.fuzzWeight = 0.0f;
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(NativePBR::ClassifyRecipeReadiness(recipe).unsupportedFeatures == 0);
	NativePBR::RecipeIssueTally tally;
	tally.Observe(recipe);
	REQUIRE(tally.affected == 0);
	REQUIRE(tally.issues[2] == 0);
	REQUIRE(tally.pbrFlags[5] == 1);
	snapshot.material.pbrFlags = uint32_t(PBRFlags::ThinSurface) | uint32_t(PBRFlags::Foliage);
	recipe = NativePBR::CompileRecipe(snapshot);
	tally.Observe(recipe);
	REQUIRE(tally.pbrFlags[7] == 1);
	REQUIRE(tally.pbrFlags[8] == 1);
	snapshot.material.pbrFlags = uint32_t(PBRFlags::Fuzz);
	snapshot.material.fuzzWeight = 0.5f;
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(NativePBR::ClassifyRecipeReadiness(recipe).unsupportedFeatures & NativePBR::UnsupportedPbrFeatures);
	counts.Observe(recipe);
	REQUIRE(counts.featureLimited == 1);
	REQUIRE(counts.opaqueFeatureCompatible == 1);
	snapshot.material.texturePaths[0] = "../invalid.dds";
	recipe = NativePBR::CompileRecipe(snapshot);
	REQUIRE(NativePBR::ClassifyRecipeReadiness(recipe).sourceIssues & NativePBR::InvalidTexturePath);
	counts.Observe(recipe);
	REQUIRE(counts.sourceLimited == 1);
}

TEST_CASE("NativePBR opaque captures ignore only projected and inactive feature roles", "[native_pbr]")
{
	NativePBR::Snapshot snapshot{ 533, 9, {} };
	snapshot.material.texturePaths = { "textures/base.dds", "textures/normal.dds", "textures/rmaos.dds", "", "", "", "" };
	snapshot.material.texturePathsComplete = true;
	snapshot.material.projectedColor = { 0.5f, 0.8f, 1.0f };
	snapshot.material.projectedGlint.enabled = true;
	auto recipe = NativePBR::CompileRecipe(snapshot);
	auto readiness = NativePBR::ClassifyRecipeReadiness(recipe);
	REQUIRE((readiness.unsupportedFeatures & (NativePBR::UnsupportedGlint | NativePBR::UnsupportedProjection)) != 0);
	CHECK(NativePBR::ResolveCapturedOpaqueFeatures(readiness, recipe, true).unsupportedFeatures == 0);
	CHECK(NativePBR::ResolveCapturedOpaqueFeatures(readiness, recipe, false).unsupportedFeatures == readiness.unsupportedFeatures);

	snapshot.material.glint.enabled = true;
	recipe = NativePBR::CompileRecipe(snapshot);
	readiness = NativePBR::ResolveCapturedOpaqueFeatures(NativePBR::ClassifyRecipeReadiness(recipe), recipe, true);
	CHECK(readiness.unsupportedFeatures == NativePBR::UnsupportedGlint);

	snapshot.material.glint.enabled = false;
	snapshot.material.texturePaths[static_cast<size_t>(NativePBR::TextureRole::Features0)] = "textures/features0.dds";
	recipe = NativePBR::CompileRecipe(snapshot);
	readiness = NativePBR::ResolveCapturedOpaqueFeatures(NativePBR::ClassifyRecipeReadiness(recipe), recipe, true);
	CHECK(readiness.unsupportedFeatures == 0);

	snapshot.material.pbrFlags = uint32_t(PBRFlags::Subsurface);
	recipe = NativePBR::CompileRecipe(snapshot);
	readiness = NativePBR::ResolveCapturedOpaqueFeatures(NativePBR::ClassifyRecipeReadiness(recipe), recipe, true);
	CHECK((readiness.unsupportedFeatures & NativePBR::UnsupportedFeatureTextures) != 0);
	CHECK((readiness.unsupportedFeatures & NativePBR::UnsupportedPbrFeatures) != 0);

	snapshot.material.pbrFlags = uint32_t(PBRFlags::Fuzz);
	snapshot.material.fuzzWeight = 0.0f;
	snapshot.material.texturePaths[static_cast<size_t>(NativePBR::TextureRole::Features1)] = "textures/features1.dds";
	recipe = NativePBR::CompileRecipe(snapshot);
	readiness = NativePBR::ResolveCapturedOpaqueFeatures(NativePBR::ClassifyRecipeReadiness(recipe), recipe, true);
	CHECK(readiness.unsupportedFeatures == NativePBR::UnsupportedFeatureTextures);
}

TEST_CASE("Material observations resolve only authoritatively known instance modes", "[native_pbr]")
{
	NativePBR::InstanceProperties opaque;
	NativePBR::ResolveObservedInstanceProperties(opaque, 1.0f);
	REQUIRE(opaque.opacity == NativePBR::InstanceProperties::Opacity::Opaque);
	REQUIRE(opaque.normalMode == NativePBR::InstanceProperties::NormalMode::TangentSpace);
	REQUIRE(opaque.uvAnimation == NativePBR::InstanceProperties::UvAnimation::Unknown);
	NativePBR::InstanceProperties model;
	model.shaderFlags = 1ull << 12;
	NativePBR::ResolveObservedInstanceProperties(model, 1.0f);
	REQUIRE(model.normalMode == NativePBR::InstanceProperties::NormalMode::ModelSpace);
	NativePBR::InstanceProperties vertexAlpha;
	vertexAlpha.shaderFlags = 1ull << 3;
	NativePBR::ResolveObservedInstanceProperties(vertexAlpha, 1.0f);
	REQUIRE(vertexAlpha.opacity == NativePBR::InstanceProperties::Opacity::NonOpaque);
	NativePBR::InstanceProperties alphaProperty;
	alphaProperty.alphaProperty = true;
	NativePBR::ResolveObservedInstanceProperties(alphaProperty, 1.0f);
	REQUIRE(alphaProperty.opacity == NativePBR::InstanceProperties::Opacity::NonOpaque);
	NativePBR::InstanceProperties missingAlpha;
	NativePBR::ResolveObservedInstanceProperties(missingAlpha, {});
	REQUIRE(missingAlpha.opacity == NativePBR::InstanceProperties::Opacity::Unknown);
	NativePBR::ResolveObservedInstanceProperties(missingAlpha, std::numeric_limits<float>::quiet_NaN());
	REQUIRE(missingAlpha.opacity == NativePBR::InstanceProperties::Opacity::Unknown);
}

TEST_CASE("Published instance readiness preserves unsupported projected and animated state", "[native_pbr]")
{
	NativePBR::Registry registry;
	NativePBR::MaterialService service;
	NativePBR::RecipeCache cache;
	int source = 0;
	NativePBR::MaterialData parameters;
	parameters.texturePaths = { "textures/c.dds", "textures/n.dds", "textures/r.dds", "", "", "", "" };
	parameters.texturePathsComplete = true;
	const auto token = registry.BeginCapture(&source);
	REQUIRE(registry.Publish(&source, token, parameters));
	NativePBR::InstanceProperties instance;
	instance.opacity = NativePBR::InstanceProperties::Opacity::Opaque;
	instance.normalMode = NativePBR::InstanceProperties::NormalMode::ModelSpace;
	instance.uvAnimation = NativePBR::InstanceProperties::UvAnimation::Animated;
	instance.projectedUv = true;
	instance.materialObjectEditorId = "missing-mato";
	registry.ObserveInstance(&source, "ref", "shape", instance);
	NativePBR::InstanceProperties opaque;
	opaque.opacity = NativePBR::InstanceProperties::Opacity::Opaque;
	opaque.normalMode = NativePBR::InstanceProperties::NormalMode::TangentSpace;
	opaque.uvAnimation = NativePBR::InstanceProperties::UvAnimation::Static;
	registry.ObserveInstance(&source, "opaque-ref", "opaque-shape", opaque);
	const auto changes = *registry.ExportChanges(0, {});
	cache.Update(changes);
	const auto generation = NativePBR::PublishRecipes(cache, changes, {});
	REQUIRE(service.Publish(generation, {}, changes.instanceHints, changes.instanceEpoch));
	const auto publication = service.GetSnapshotForRegistry(registry);
	REQUIRE(publication);
	const auto material = publication->FindMaterial(token.identity);
	REQUIRE(material.Readiness().unresolvedInstance == NativePBR::kUnresolvedInstanceIssues);
	const auto bound = publication->FindInstance("ref", "shape");
	REQUIRE(bound);
	REQUIRE(bound.recipe == material.recipe);
	REQUIRE(bound.Readiness().unresolvedInstance == 0);
	REQUIRE(bound.Readiness().unsupportedFeatures == 0);
	REQUIRE(bound.instanceIssues & NativePBR::UnsupportedModelSpaceNormal);
	REQUIRE(bound.instanceIssues & NativePBR::UnsupportedAnimatedUv);
	REQUIRE(bound.instanceIssues & NativePBR::UnsupportedProjectedInstance);
	REQUIRE(bound.instanceIssues & NativePBR::MissingMatoConfiguration);
	REQUIRE(bound.missingMatoEditorId == "missing-mato");
	REQUIRE(material.recipe->revision == bound.recipe->revision);
	const auto safe = publication->FindInstance("opaque-ref", "opaque-shape");
	REQUIRE(safe);
	REQUIRE(safe.recipe->identity == token.identity);
	REQUIRE(safe.recipe->revision == material.recipe->revision);
	REQUIRE(safe.Readiness().IsOpaqueFeatureCompatible());
	REQUIRE(safe.Readiness().unresolvedInstance == 0);
	REQUIRE(safe.instanceIssues == 0);
}
