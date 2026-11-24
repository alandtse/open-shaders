#include "Features/DX12Resources/PgReferenceRegistry.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("PG material references publish as an immutable validated snapshot", "[dx12_resources]")
{
	const std::string hash(64, 'a');
	nlohmann::json document = {
		{ "kind", "dx12-pg-references" }, { "schemaVersion", 1 },
		{ "sourceDiffSha256", hash }, { "meshCount", 1 }, { "shapeCount", 1 },
		{ "cacheHits", 1 }, { "unresolved", 0 },
		{ "meshes", nlohmann::json::array({ {
			{ "path", "meshes\\landscape\\snow.nif" }, { "status", "verified" },
			{ "sha256", hash }, { "expectedCrc32", 123u },
			{ "shapes", nlohmann::json::array({ {
				{ "blockIndex", 12u }, { "shaderFlags1", 3u },
				{ "shaderFlags2", 5u }, { "textures", nlohmann::json::array({ "textures\\snow\\snow_d.dds" }) }
			} }) }
		} }) }
	};
	const auto snapshot = DX12ResourceService::ParsePgReferenceSnapshot(document);
	REQUIRE(snapshot->meshCount == 1);
	REQUIRE(snapshot->shapeCount == 1);
	REQUIRE(snapshot->FindMesh("Landscape/SNOW.nif") != nullptr);
	REQUIRE(snapshot->FindMesh("meshes/landscape/snow.nif") != nullptr);
	REQUIRE(snapshot->FindMesh("other.nif") == nullptr);
	REQUIRE(snapshot->FindMesh("../snow.nif") == nullptr);
	document["meshes"][0]["shapes"][0]["shaderFlags1"] = 8u;
	REQUIRE(snapshot->FindMesh("Landscape/SNOW.nif")->at("shapes")[0]["shaderFlags1"] == 3u);

	SECTION("reject a missing shape")
	{
		document["shapeCount"] = 2;
		REQUIRE_THROWS(DX12ResourceService::ParsePgReferenceSnapshot(document));
	}
	SECTION("reject a traversal path")
	{
		document["meshes"][0]["path"] = "meshes\\..\\unsafe.nif";
		REQUIRE_THROWS(DX12ResourceService::ParsePgReferenceSnapshot(document));
	}
	SECTION("reject a changed PG source contract")
	{
		document["schemaVersion"] = 2;
		REQUIRE_THROWS(DX12ResourceService::ParsePgReferenceSnapshot(document));
	}
}
