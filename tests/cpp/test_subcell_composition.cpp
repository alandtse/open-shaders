#include "Features/DX12NativeScene/SubcellSceneComposition.h"
#include <catch2/catch_test_macros.hpp>

using namespace DX12Native;

static std::shared_ptr<PreparedCell> MockTile(const char* name, const std::shared_ptr<Mesh>& mesh)
{
	auto tile = std::make_shared<PreparedCell>();
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->offline = true;
	snapshot->scope.scene = 7;
	auto source = std::make_shared<MaterialSource>();
	source->cell = "000001:Skyrim.esm";
	source->textureReferences = std::make_shared<const json>(json::array());
	snapshot->materialSource = source;
	auto geometry = std::make_shared<Geometry>();
	geometry->source = mesh;
	geometry->allocationBytes = 128;
	tile->geometries.emplace(mesh.get(), geometry);
	Instance instance{};
	instance.mesh = mesh;
	instance.preparedIdentity.referenceKey = name;
	instance.preparedIdentity.shapeName = "static";
	instance.preparedIdentity.blockIndex = 0;
	snapshot->instances.push_back(instance);
	tile->snapshot = snapshot;
	return tile;
}

TEST_CASE("Subcell composition deduplicates a shared reference without replicating geometry", "[dx12-native-scene][subcells]")
{
	auto mesh = std::make_shared<Mesh>();
	auto first = MockTile("REF-A", mesh);
	auto repeated = MockTile("REF-A", mesh);
	auto additional = MockTile("REF-B", mesh);
	std::array<std::shared_ptr<const PreparedCell>, 3> tiles{ first, repeated, additional };
	const auto composed = ComposeActiveSubcells(tiles, 7, 123);
	REQUIRE(composed.cell);
	CHECK(composed.sources == 3);
	CHECK(composed.admittedInstances == 2);
	CHECK(composed.duplicateInstances == 1);
	CHECK(composed.uniqueGeometries == 1);
	CHECK(composed.cell->bytes == 128);
	CHECK(composed.cell->snapshot->scope.policy == 3);
	CHECK(composed.cell->snapshot->instances[1].identity == 2);
}
