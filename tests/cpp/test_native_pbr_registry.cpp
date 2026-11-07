#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "TruePBR/NativePBRRegistry.h"
#include "TruePBR/NativePBRCompiler.h"
#include "TruePBR/InstanceCoverage.h"
#include "Features/DX12NativeScene/LogicalTextureRegistry.h"

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
