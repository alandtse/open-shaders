#include "Features/NativePBR/GpuMaterialAbi.h"
#include "Features/NativePBR/GpuMaterialResidency.h"
#include "Features/NativePBR/GpuMaterialResources.h"
#include "Features/NativePBR/RayMaterialBindings.h"
#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("NativePBR GPU material ABI preserves all seven texture roles", "[NativePBR][gpu-material]")
{
	NativePBR::Recipe recipe;
	recipe.identity = 0x1122334455667788ull;
	recipe.revision = 0x99aabbccddeeff00ull;
	recipe.issues = NativePBR::UnsupportedDisplacement | NativePBR::UnsupportedFeatureTextures;
	recipe.parameters.roughnessScale = 0.67f;
	recipe.parameters.displacementScale = 1.7f;
	recipe.parameters.projectedColor = { 0.2f, 0.3f, 0.4f };
	recipe.parameters.glint.enabled = true;
	NativePBR::GpuTextureLocations locations{};
	for (size_t role = 0; role < locations.size(); ++role)
		locations[role] = { uint32_t(role + 10), uint32_t(role + 20) };
	locations[4] = { NativePBR::kGpuMissingTexture, 0 };

	const auto record = NativePBR::PackGpuMaterialRecord(recipe, locations);
	CHECK((record.textures[0] == std::array<uint32_t, 4>{ 10, 20, 11, 21 }));
	CHECK((record.textures[1] == std::array<uint32_t, 4>{ 12, 22, 13, 23 }));
	CHECK((record.textures[2] == std::array<uint32_t, 4>{ NativePBR::kGpuMissingTexture, 0, 15, 25 }));
	CHECK((record.textures[3] == std::array<uint32_t, 4>{ 16, 26, NativePBR::kGpuMissingTexture, 0 }));
	CHECK(record.controls[0] == 0x6fu);
	CHECK(record.controls[1] == 1u);
	CHECK(record.properties[3] == recipe.issues);
	CHECK(record.base[1] == 0.67f);
	CHECK(record.base[2] == 1.7f);
	CHECK(record.projected[2] == 0.4f);
	CHECK((record.identity == std::array<uint32_t, 4>{ 0x55667788u, 0x11223344u, 0xddeeff00u, 0x99aabbccu }));
}

TEST_CASE("NativePBR GPU records distinguish unresident sources from missing material identities", "[NativePBR][gpu-material]")
{
	NativePBR::Recipe recipe;
	recipe.identity = 25;
	recipe.revision = 3;
	NativePBR::GpuTextureLocations locations{};
	for (auto& location : locations)
		location = { NativePBR::kGpuMissingTexture, 0 };
	const auto record = NativePBR::PackGpuMaterialRecord(recipe, locations);
	CHECK(record.controls[0] == 0u);
	CHECK(record.identity[0] == 25u);
	CHECK(record.identity[2] == 3u);
	CHECK(record.textures[3][2] == NativePBR::kGpuMissingTexture);
}

TEST_CASE("NativePBR GPU draw bindings preserve full material identity and revision", "[NativePBR][gpu-material]")
{
	const auto binding = NativePBR::PackGpuDrawBinding(0x1122334455667788ull,
		0x99aabbccddeeff00ull, 17u, NativePBR::kGpuDrawProjected | NativePBR::kGpuDrawUsesVertexColor);
	CHECK((binding.identity == std::array<uint32_t, 2>{ 0x55667788u, 0x11223344u }));
	CHECK((binding.revision == std::array<uint32_t, 2>{ 0xddeeff00u, 0x99aabbccu }));
	CHECK(binding.materialIndex == 17u);
	CHECK(binding.flags == 3u);
}

TEST_CASE("NativePBR packs dynamic snow coverage without changing draw identity", "[NativePBR][snow]")
{
	using namespace NativePBR;
	constexpr uint32_t snowCandidate = kGpuDrawPreparedOpaque | kGpuDrawProjectedSnowCandidate;
	const auto half = WithNativeSnowCoverage(snowCandidate | kGpuDrawUsesVertexColor, 0.5f);
	CHECK((half & 0xffu) == (snowCandidate | kGpuDrawUsesVertexColor));
	CHECK(((half & kGpuSnowCoverageMask) >> kGpuSnowCoverageShift) == 128u);
	CHECK(((WithNativeSnowCoverage(half, 1.0f) & kGpuSnowCoverageMask) >> kGpuSnowCoverageShift) == 255u);
	CHECK(((WithNativeSnowCoverage(half, -1.0f) & kGpuSnowCoverageMask) >> kGpuSnowCoverageShift) == 0u);
	CHECK(((WithNativeSnowCoverage(half, std::numeric_limits<float>::quiet_NaN()) & kGpuSnowCoverageMask) >> kGpuSnowCoverageShift) == 0u);
	CHECK(NativeSnowCoverage(1.0f, false) == 1.0f);
	CHECK(NativeSnowCoverage(0.5f, false) == 0.5f);
	CHECK(NativeSnowCoverage(0.0f, false) == 0.0f);
	CHECK(NativeSnowCoverage(0.0f, true) == 0.55f);
	CHECK(NativeSnowCoverage(1.0f, true) == 1.0f);
}

TEST_CASE("NativePBR GPU descriptor ABI keeps linear and sRGB views within 512 slots", "[NativePBR][gpu-material]")
{
	using namespace NativePBR;
	CHECK(sizeof(GpuMaterialRecord) == 304);
	CHECK(sizeof(GpuDrawBinding) == 24);
	CHECK(GpuMaterialView::kRecordStride == 304);
	CHECK(GpuMaterialView::kDescriptorCount == 512);
	CHECK(GpuTextureDescriptorIndex(0, TextureInterpretation::DataLinear) == 0);
	CHECK(GpuTextureDescriptorIndex(255, TextureInterpretation::DataLinear) == 255);
	CHECK(GpuTextureDescriptorIndex(0, TextureInterpretation::ColorSrgb) == 256);
	CHECK(GpuTextureDescriptorIndex(255, TextureInterpretation::ColorSrgb) == 511);
	CHECK(GpuTextureDescriptorIndex(256, TextureInterpretation::ColorSrgb) == kGpuMissingTexture);
	CHECK(GpuTextureDescriptorIndex(UINT32_MAX, TextureInterpretation::DataLinear) == kGpuMissingTexture);
}

TEST_CASE("NativePBR material availability requires only sampled opaque roles", "[NativePBR][gpu-material]")
{
	using namespace NativePBR;
	Recipe recipe;
	recipe.identity = 11;
	recipe.issues = UnsupportedDisplacement | UnsupportedFeatureTextures;
	recipe.textures[3].canonicalPath = "textures/emission.dds";
	recipe.textures[4].canonicalPath = "textures/displacement.dds";
	recipe.textures[5].canonicalPath = "textures/features0.dds";
	const auto requested = GpuRequestedTextureRoles(recipe);
	CHECK(requested == 0x1fu);
	CHECK(RayRequiredTextureRoles(requested, false) == 0x0fu);
	CHECK(RayRequiredTextureRoles(requested, true) == requested);
	CHECK(ClassifyGpuMaterialAvailability(0x0fu, RayRequiredTextureRoles(requested, false)) ==
		GpuMaterialAvailability::Resident);
	CHECK(ClassifyGpuMaterialAvailability(0x1fu, requested) == GpuMaterialAvailability::Resident);
	CHECK(ClassifyGpuMaterialAvailability(0x0fu, requested) == GpuMaterialAvailability::PartiallyResident);
	CHECK(ClassifyGpuMaterialAvailability(0x17u, requested) == GpuMaterialAvailability::PartiallyResident);
	CHECK(ClassifyGpuMaterialAvailability(0x07u, requested) == GpuMaterialAvailability::PartiallyResident);
	CHECK(ClassifyGpuMaterialAvailability(0x04u, requested) == GpuMaterialAvailability::PartiallyResident);
	CHECK(ClassifyGpuMaterialAvailability(0, requested) == GpuMaterialAvailability::Unavailable);
	recipe.textures[3].canonicalPath.clear();
	CHECK(GpuRequestedTextureRoles(recipe) == (kGpuRequiredTextureRoles | (1u << 4u)));
	CHECK(ClassifyGpuMaterialAvailability(0x07u, GpuRequestedTextureRoles(recipe)) == GpuMaterialAvailability::PartiallyResident);
	CHECK(ClassifyGpuMaterialAvailability(0x17u, GpuRequestedTextureRoles(recipe)) == GpuMaterialAvailability::Resident);
	CHECK(recipe.issues == (UnsupportedDisplacement | UnsupportedFeatureTextures));
}

TEST_CASE("NativePBR classifies resident displacement and complex mask without altering the material ABI", "[NativePBR][gpu-material]")
{
	using namespace NativePBR;
	Snapshot snapshot{ 321, 12, {} };
	snapshot.material.texturePathsComplete = true;
	snapshot.material.texturePaths = { "textures/diffuse.dds", "textures/normal.dds", "textures/environment.dds",
		"", "textures/height.dds", "", "" };
	snapshot.material.legacyComplexMask = true;
	const auto recipe = CompileRecipe(snapshot);
	CHECK((recipe.issues & UnsupportedDisplacement) != 0);
	CHECK((EffectiveNativeIssues(recipe) & UnsupportedDisplacement) == 0);
	CHECK((GpuRequestedTextureRoles(recipe) & (1u << static_cast<uint32_t>(TextureRole::Displacement))) != 0);
	GpuTextureLocations locations{};
	for (auto& location : locations)
		location = { kGpuMissingTexture, 0u };
	const auto packed = PackGpuMaterialRecord(recipe, locations);
	CHECK((packed.controls[1] & kGpuLegacyComplexMask) != 0);
	CHECK(sizeof(packed) == 304);
	snapshot.material.displacementScale = 0.0f;
	const auto flatRecipe = CompileRecipe(snapshot);
	CHECK((EffectiveNativeIssues(flatRecipe) & UnsupportedDisplacement) != 0);
	CHECK((GpuRequestedTextureRoles(flatRecipe) & (1u << static_cast<uint32_t>(TextureRole::Displacement))) == 0);
	snapshot.material.displacementScale = 1.0f;
	snapshot.material.pbrFlags = uint32_t(PBRFlags::TwoLayer) | uint32_t(PBRFlags::InterlayerParallax);
	snapshot.material.coatStrength = 1.0f;
	const auto layeredRecipe = CompileRecipe(snapshot);
	CHECK_FALSE(SupportsOpaqueParallax(layeredRecipe));
	CHECK((EffectiveNativeIssues(layeredRecipe) & UnsupportedDisplacement) != 0);
	const auto layeredRecord = PackGpuMaterialRecord(layeredRecipe, locations);
	CHECK((layeredRecord.controls[1] & kGpuLegacyComplexMask) == 0);
}

TEST_CASE("NativePBR clearcoat requests its geometry-sampled feature texture", "[NativePBR][gpu-material]")
{
	using namespace NativePBR;
	Recipe recipe;
	recipe.parameters.pbrFlags = uint32_t(PBRFlags::TwoLayer);
	recipe.parameters.coatStrength = 0.8f;
	recipe.textures[static_cast<size_t>(TextureRole::Features0)].canonicalPath = "textures/coat.dds";
	CHECK(SupportsOpaqueClearcoat(recipe));
	CHECK(GpuRequestedTextureRoles(recipe) == (kGpuRequiredTextureRoles | (1u << 5u)));
	CHECK(ClassifyGpuMaterialAvailability(0x07u, GpuRequestedTextureRoles(recipe)) ==
		GpuMaterialAvailability::PartiallyResident);
	CHECK(ClassifyGpuMaterialAvailability(0x27u, GpuRequestedTextureRoles(recipe)) ==
		GpuMaterialAvailability::Resident);
	GpuTextureLocations locations{};
	for (auto& location : locations)
		location = { kGpuMissingTexture, 0 };
	const auto record = PackGpuMaterialRecord(recipe, locations);
	CHECK(record.controls[3] == uint32_t(PBRFlags::TwoLayer));
	CHECK(record.coat[0] == 0.8f);
	CHECK(record.coat[2] == 0.04f);
}

TEST_CASE("NativePBR retained DDS pages are remapped without copying unrelated pages", "[NativePBR][gpu-material]")
{
	const std::unordered_set<std::string> required{ "textures\\base.dds", "textures\\other.dds" };
	const std::unordered_map<std::string, uint64_t> locations{
		{ "textures\\base.dds", 101 }, { "textures\\old.dds", 102 },
		{ "textures\\other.dds", 103 }
	};
	const std::unordered_map<uint64_t, std::array<uint32_t, 2>> logical{
		{ 101, { 2, 0 } }, { 102, { 1, 0 } }, { 103, { 0, 4 } }
	};
	const auto remap = NativePBR::PlanGpuMaterialPageRemap(required, locations, logical, 3);
	CHECK((remap == std::vector<uint32_t>{ 0, UINT32_MAX, 1 }));
	CHECK((NativePBR::PlanGpuMaterialPageRemap({}, locations, logical, 3) ==
		std::vector<uint32_t>{ UINT32_MAX, UINT32_MAX, UINT32_MAX }));
}

TEST_CASE("NativePBR GPU material retirement waits for the consumer fence value", "[NativePBR][gpu-material]")
{
	CHECK_FALSE(NativePBR::GpuMaterialRetirement::Completed(4, 5));
	CHECK(NativePBR::GpuMaterialRetirement::Completed(5, 5));
	CHECK(NativePBR::GpuMaterialRetirement::Completed(8, 5));
	CHECK(NativePBR::GpuMaterialRetirement::Completed(UINT64_MAX, 5));
}

TEST_CASE("NativePBR material generations retain provenance only for active DDS paths", "[NativePBR][gpu-material]")
{
	DX12Native::VerifiedTextureSources sources;
	REQUIRE(sources.Observe("textures/a.dds", std::string(64, 'a'), 256, 256, 7));
	REQUIRE(sources.Observe("textures/b.dds", std::string(64, 'b'), 512, 512, 8));
	const auto selected = sources.Select({ "textures/b.dds" });
	CHECK(sources.Size() == 2);
	CHECK(selected.Size() == 1);
	NativePBR::Recipe recipe;
	recipe.textures[0].canonicalPath = "textures/b.dds";
	CHECK(selected.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Available);
	recipe.textures[0].canonicalPath = "textures/a.dds";
	CHECK(selected.Assess(recipe) == DX12Native::LiveTextureSourceStatus::Missing);
}

TEST_CASE("NativePBR ray bindings follow the exact scene and resident material generation", "[NativePBR][gpu-material]")
{
	using namespace NativePBR;
	auto recipes = std::make_shared<RecipeGeneration>();
	auto recipe = std::make_shared<Recipe>();
	recipe->identity = 42;
	recipe->revision = 7;
	recipes->recipes.emplace(42, recipe);
	auto publication = std::make_shared<MaterialPublication>();
	publication->recipes = recipes;
	auto& observed = publication->instances[MaterialPublication::Key("Ref", "Mesh")];
	observed.materialIdentity = 42;
	observed.properties.opacity = InstanceProperties::Opacity::Opaque;
	observed.properties.normalMode = InstanceProperties::NormalMode::TangentSpace;
	DX12Native::Instance instance;
	instance.identity = 100;
	instance.preparedIdentity.referenceKey = "Ref";
	instance.preparedIdentity.shapeName = "Mesh";
	instance.preparedIdentity.sourceDigest = "companion-shape-digest";
	instance.material.offlineDescription = std::make_shared<nlohmann::json>(nlohmann::json{ { "shaderFlags2", 1u << 5 } });
	auto attributes = std::make_shared<DX12Native::VertexAttributes>();
	attributes->stride = 60;
	attributes->hasTangentFrame = true;
	instance.material.attributes = attributes;
	auto scene = std::make_shared<DX12Native::TopLevel>();
	scene->instances = { instance };
	auto resources = std::make_shared<DX12Native::ShadowBindings>();
	resources->scene = scene;
	resources->materials = std::make_shared<DX12Native::MaterialResources>();
	const auto prepared = PrepareSceneMaterials(scene, resources, publication, 12, 9);
	REQUIRE(prepared);
	auto gpu = std::make_shared<GpuMaterialResources>();
	gpu->generation = recipes;
	gpu->indices.emplace(42, 0);
	gpu->entries.push_back({ 0, 7, 7, 0, 7, GpuMaterialAvailability::Resident });
	const auto bound = PrepareRayMaterialBindings(prepared, gpu);
	REQUIRE(bound);
	REQUIRE(bound->eligible == 1);
	CHECK(bound->instances[0].materialIndex == 0);
	CHECK((bound->instances[0].identity == std::array<uint32_t, 2>{ 42, 0 }));
	CHECK((bound->instances[0].revision == std::array<uint32_t, 2>{ 7, 0 }));
	CHECK(bound->instances[0].flags == (kGpuDrawUsesVertexColor | kGpuDrawPreparedOpaque));
	gpu->entries[0].availability = GpuMaterialAvailability::PartiallyResident;
	const auto partial = PrepareRayMaterialBindings(prepared, gpu);
	REQUIRE(partial);
	CHECK(partial->eligible == 0);
	CHECK(partial->instances[0].materialIndex == UINT32_MAX);
	CHECK(partial->instances[0].flags == kGpuDrawPreparedOpaque);
	const auto forced = PrepareRayMaterialBindings(prepared, gpu, true);
	REQUIRE(forced);
	CHECK(forced->forcedAdmission);
	CHECK(forced->eligible == 1);
	CHECK(forced->instances[0].materialIndex == 0);
	gpu->entries[0].residentRoles = 0x07u;
	gpu->entries[0].requestedRoles = 0x17u;
	const auto baseOnly = PrepareRayMaterialBindings(prepared, gpu, false, false);
	REQUIRE(baseOnly);
	CHECK_FALSE(baseOnly->parallaxRequired);
	CHECK(baseOnly->eligible == 1);
	const auto withParallax = PrepareRayMaterialBindings(prepared, gpu, false, true);
	REQUIRE(withParallax);
	CHECK(withParallax->eligible == 0);
	attributes->stride = 36;
	CHECK_FALSE(RayOpaqueEligible(scene->instances[0], prepared->instances[0].material));
	CHECK(RayOpaqueEligible(scene->instances[0], prepared->instances[0].material, true));
	auto unrelated = std::make_shared<RecipeGeneration>();
	gpu->generation = unrelated;
	CHECK_FALSE(PrepareRayMaterialBindings(prepared, gpu));
	CHECK_FALSE(PrepareRayMaterialBindings(prepared, gpu, true));
}
