#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "TruePBR/NativePBRRegistry.h"
#include "TruePBR/NativePBRCompiler.h"
#include "TruePBR/InstanceCoverage.h"
#include "TruePBR/VerifiedBridge.h"
#include "Features/DX12NativeScene/LiveLooseTexturePlan.h"
#include "Features/DX12NativeScene/RuntimeTextureProgress.h"
#include "Features/DX12NativeScene/MaterialTextureLookup.h"
#include "Features/DX12NativeScene/ArchiveDdsSource.h"
#include "Features/DX12NativeScene/LiveGpuTextureEligibility.h"
#include "Features/DX12NativeScene/RuntimeMaterialCompiler.h"
#include "Features/DX12NativeScene/LogicalTextureRegistry.h"
#include "Features/DX12NativeScene/MaterialPageCache.h"
#include "Features/DX12NativeScene/VerifiedTextureSources.h"
#include "Features/DX12NativeScene/MaterialDiskCache.h"
#include "Features/DX12NativeScene/TextureSourceBatch.h"
#include "Features/DX12NativeScene/MaterialUploadStatus.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include <fstream>

TEST_CASE("Persistent material cache verifies content and bounds storage", "[native_pbr]")
{
    const auto root = std::filesystem::current_path() / ("native-pbr-cache-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    const std::string first = "e1b03360f3a3c6bd52b4511bc0279f07b38a1b1bb7ab209d31833f7617e2762f";
    const std::string second = "0b6def3dcedfcb01eddc4ff76236225c8f66912ac8346c84e2e1fc1a00e61550";
    struct Cleanup {
        std::filesystem::path root;
        std::string first, second;
        ~Cleanup() { std::error_code error; std::filesystem::remove(root / (first + ".dds"), error); std::filesystem::remove(root / (second + ".dds"), error); std::filesystem::remove(root / (first + ".tmp.1.1"), error); std::filesystem::remove(root / ".lock", error); std::filesystem::remove(root, error); }
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
    { std::ofstream corrupt(root / (second + ".dds"), std::ios::binary | std::ios::trunc); corrupt << "corrupt"; }
    REQUIRE_FALSE(cache.Read(second, 128));
    REQUIRE_FALSE(cache.Known(second));
    REQUIRE(cache.Store(second, b) == DX12Native::MaterialDiskCache::StoreResult::Stored);
    REQUIRE(cache.Read(second, 128) == b);
    REQUIRE_FALSE(cache.Read("../invalid", 128));
    const auto stale = root / (first + ".tmp.1.1");
    { std::ofstream temp(stale, std::ios::binary); temp.write(reinterpret_cast<const char*>(a.data()), a.size()); }
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
        loaded.push_back(entry);
    }, [&](const std::exception&) { ++rejected; });
    REQUIRE(loaded == std::vector<int>{ 1, 3 });
    REQUIRE(rejected == 1);
    REQUIRE_THROWS_AS(DX12Native::ProcessTextureSources(entries, [](int) { throw std::bad_alloc(); },
        [](const std::exception&) {}), std::bad_alloc);
    REQUIRE_THROWS_AS(DX12Native::ProcessTextureSources(entries, [](int) { throw std::runtime_error("device"); },
        [](const std::exception&) { throw; }), std::runtime_error);
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
    REQUIRE(pending->pendingIdentities == std::vector<uint64_t>{first->snapshots.front().identity});
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
    REQUIRE(pending->pendingIdentities == std::vector<uint64_t>{capture.identity});
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
    REQUIRE(sources.Observe("textures/a.dds", a, 256, 256, 8)); // canonical duplicate
    REQUIRE_FALSE(sources.Observe("textures/a.dds", b, 256, 256, 8));
    REQUIRE(sources.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Ambiguous);

    DX12Native::VerifiedTextureSources distinct;
    REQUIRE(distinct.Observe("textures/a.dds", a, 256, 256, 8));
    REQUIRE(distinct.Observe("textures/n.dds", a, 256, 256, 8));
    REQUIRE(distinct.Observe("textures/r.dds", b, 256, 256, 8));
    recipe.textures[static_cast<size_t>(NativePBR::TextureRole::Normal)].resource =
        { 128, 256, 8, 1, 0, 0, 0, 0, 1, true };
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
    properties.shaderFlags = 1ull << 37; // runtime vertex colors
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
    snapshot.material.pbrFlags = (1u << 0) | (1u << 5); // subsurface and fuzz
    snapshot.material.glint.enabled = true;
    const auto recipe = NativePBR::CompileRecipe(snapshot);
    NativePBR::RecipeIssueTally counts;
    counts.Observe(recipe);
    REQUIRE(counts.affected == 1);
    REQUIRE(counts.issues[2] == 1);
    REQUIRE(counts.issues[3] == 1);
    REQUIRE(counts.issues[4] == 0); // UnknownOpacity is a baseline hint
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
    limitedSnapshot.material.pbrFlags = 1u << 5; // fuzz requires a dedicated shading implementation
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
    snapshot.material.pbrFlags |= 1u << 1; // active coat: still unsupported
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
