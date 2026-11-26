#include "Features/DX12/ScenePrepMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

using Catch::Approx;

TEST_CASE("Vertex descriptor helpers use packed dword stride and position bit", "[dx12][scene-prep]")
{
	using DX12::HasVertexPositionStream;
	using DX12::kVertexPositionFlagBit;
	using DX12::VertexStrideBytes;

	constexpr uint64_t kLandscapeDesc = kVertexPositionFlagBit | (uint64_t{ 1u << 7 } << 44) | 9ull;
	CHECK(HasVertexPositionStream(kLandscapeDesc));
	CHECK(VertexStrideBytes(kLandscapeDesc) == 36u);

	constexpr uint64_t kNoPosDesc = 4ull;
	CHECK_FALSE(HasVertexPositionStream(kNoPosDesc));
	CHECK(VertexStrideBytes(kNoPosDesc) == 16u);
}

TEST_CASE("ResolveMeshLODSlice handles progressive and single-level LOD counts", "[dx12][scene-prep]")
{
	using DX12::ResolveMeshLODSlice;

	const std::array<uint32_t, 3> counts{ 4u, 2u, 1u };
	const auto progressive = ResolveMeshLODSlice(0x02u, counts, 7u);
	REQUIRE(progressive);
	CHECK(progressive->first == 0u);
	CHECK(progressive->second == 21u);

	const auto singleLod0 = ResolveMeshLODSlice(0x82u, counts, 7u);
	REQUIRE(singleLod0);
	CHECK(singleLod0->first == 0u);
	CHECK(singleLod0->second == 12u);

	const std::array<uint32_t, 3> missingLod0{ 0u, 3u, 1u };
	const auto singleLod1 = ResolveMeshLODSlice(0x81u, missingLod0, 4u);
	REQUIRE(singleLod1);
	CHECK(singleLod1->first == 0u);
	CHECK(singleLod1->second == 9u);

	CHECK_FALSE(ResolveMeshLODSlice(0x83u, missingLod0, 4u));
	CHECK_FALSE(ResolveMeshLODSlice(0x82u, std::array<uint32_t, 3>{ 0u, 2u, 5u }, 4u));
	CHECK_FALSE(ResolveMeshLODSlice(0x00u, std::array<uint32_t, 3>{ 0u, 0u, 0u }, 0u));
}

TEST_CASE("DecodeRawMeshBuffers unpacks strided vertices and validates indices", "[dx12][scene-prep]")
{
	using DX12::DecodeRawMeshBuffers;

	constexpr uint32_t kStride = 36u;
	std::vector<uint8_t> rawVerts(3u * kStride, 0xCCu);
	const std::array<float3, 3> expectedPos{
		float3{ 1.0f, 2.0f, 3.0f },
		float3{ -4.0f, 5.5f, 6.0f },
		float3{ 7.0f, -8.0f, 9.25f }
	};
	for (size_t i = 0; i < expectedPos.size(); ++i)
		std::memcpy(rawVerts.data() + i * kStride, &expectedPos[i], sizeof(float3));

	const std::array<uint16_t, 6> rawIndices{ 0, 1, 2, 2, 1, 0 };
	std::vector<float3> positions;
	std::vector<uint32_t> indices;

	REQUIRE(DecodeRawMeshBuffers(rawVerts.data(), 3u, kStride, rawIndices.data(), 3u, 3u, positions, indices));
	REQUIRE(positions.size() == 3u);
	CHECK(positions[0].x == 1.0f);
	CHECK(positions[1].y == 5.5f);
	CHECK(positions[2].z == 9.25f);
	CHECK(indices == std::vector<uint32_t>{ 2u, 1u, 0u });

	const std::array<uint16_t, 3> badIndices{ 0, 3, 1 };
	CHECK_FALSE(DecodeRawMeshBuffers(rawVerts.data(), 3u, kStride, badIndices.data(), 0u, 3u, positions, indices));
	CHECK_FALSE(DecodeRawMeshBuffers(rawVerts.data(), 3u, 12u, rawIndices.data(), 0u, 3u, positions, indices));
	CHECK_FALSE(DecodeRawMeshBuffers(rawVerts.data(), 3u, kStride, rawIndices.data(), 0u, 4u, positions, indices));

	const float nanVal = std::numeric_limits<float>::quiet_NaN();
	std::memcpy(rawVerts.data() + kStride + 4, &nanVal, sizeof(float));
	CHECK_FALSE(DecodeRawMeshBuffers(rawVerts.data(), 3u, kStride, rawIndices.data(), 0u, 3u, positions, indices));
}

TEST_CASE("Affine placement, composition, and 3x4 inversion checks match NiTransform", "[dx12][scene-prep]")
{
	using DX12::ComposeAffine;
	using DX12::IsInvertibleTransform3x4;
	using DX12::PlacementFromEulerXYZ;
	using DX12::TransformToRows3x4;

	constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
	const auto parent = PlacementFromEulerXYZ({ 100.0f, 200.0f, 300.0f }, { 0.0f, 0.0f, kHalfPi }, 2.0f);
	REQUIRE(parent);

	const auto child = PlacementFromEulerXYZ({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.5f);
	REQUIRE(child);

	const auto combined = ComposeAffine(*parent, *child);
	const auto rows = TransformToRows3x4(combined);
	CHECK(IsInvertibleTransform3x4(rows));
	CHECK(rows[0] == Approx(0.0f).margin(1e-5f));
	CHECK(rows[1] == Approx(1.0f).margin(1e-5f));
	CHECK(rows[4] == Approx(-1.0f).margin(1e-5f));
	CHECK(rows[5] == Approx(0.0f).margin(1e-5f));
	CHECK(rows[10] == Approx(1.0f).margin(1e-5f));
	CHECK(rows[3] == Approx(100.0f).margin(1e-4f));
	CHECK(rows[7] == Approx(198.0f).margin(1e-4f));
	CHECK(rows[11] == Approx(300.0f).margin(1e-4f));

	CHECK_FALSE(PlacementFromEulerXYZ({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f));
	CHECK_FALSE(PlacementFromEulerXYZ({ std::numeric_limits<float>::infinity(), 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 1.0f));

	std::array<float, 12> singular{};
	CHECK_FALSE(IsInvertibleTransform3x4(singular));
}

TEST_CASE("NormalizeNifPath enforces meshes prefix and rejects traversal", "[dx12][scene-prep]")
{
	using DX12::NormalizeNifPath;

	CHECK(NormalizeNifPath("  Architecture/Whiterun/Tower.nif  ") == "meshes\\Architecture\\Whiterun\\Tower.nif");
	CHECK(NormalizeNifPath("MeShEs\\Clutter\\Pot01.NIF") == "meshes\\Clutter\\Pot01.NIF");

	CHECK_FALSE(NormalizeNifPath(""));
	CHECK_FALSE(NormalizeNifPath("meshes\\..\\secret.nif"));
	CHECK_FALSE(NormalizeNifPath("meshes\\.\\pot.nif"));
	CHECK_FALSE(NormalizeNifPath("meshes\\\\pot.nif"));
	CHECK_FALSE(NormalizeNifPath("\\meshes\\pot.nif"));
	CHECK_FALSE(NormalizeNifPath("C:\\meshes\\pot.nif"));
	CHECK_FALSE(NormalizeNifPath("meshes\\pot.dds"));
	CHECK_FALSE(NormalizeNifPath("meshes\\bad\npath.nif"));
	CHECK_FALSE(NormalizeNifPath("meshes\\" + std::string(260, 'a') + ".nif"));
}

TEST_CASE("Subcell overlap, cell windows, and shape identities handle negative tiles", "[dx12][scene-prep]")
{
	using DX12::InCandidateHaloXY;
	using DX12::InsideSubcellXY;
	using DX12::MakeShapeIdentity;
	using DX12::OverlapsSubcellXY;
	using DX12::SubcellExteriorCellWindow;

	CHECK(InsideSubcellXY(0.0, 0.0, 0, 0, 4096));
	CHECK_FALSE(InsideSubcellXY(4096.0, 0.0, 0, 0, 4096));
	CHECK(InsideSubcellXY(-1.0, -4096.0, -1, -1, 4096));
	CHECK(InCandidateHaloXY(-8192.0, 12287.0, 0, 0, 4096, 2));
	CHECK_FALSE(InCandidateHaloXY(-8193.0, 0.0, 0, 0, 4096, 2));

	const std::array<float, 12> crossingTransform{
		1.0f, 0.0f, 0.0f, -10.0f,
		0.0f, 1.0f, 0.0f, 100.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
	const std::vector<float3> meshVerts{ { -5.0f, 0.0f, 0.0f }, { 25.0f, 10.0f, 0.0f } };
	const auto overlaps = OverlapsSubcellXY(meshVerts, crossingTransform, 0, 0, 4096);
	REQUIRE(overlaps);
	CHECK(*overlaps);

	const std::array<float, 12> farTransform{
		1.0f, 0.0f, 0.0f, -100.0f,
		0.0f, 1.0f, 0.0f, 100.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
	const auto disjoint = OverlapsSubcellXY(meshVerts, farTransform, 0, 0, 4096);
	REQUIRE(disjoint);
	CHECK_FALSE(*disjoint);

	const std::vector<float3> badVerts{ { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f } };
	CHECK_FALSE(OverlapsSubcellXY(badVerts, crossingTransform, 0, 0, 4096));

	const auto w4k = SubcellExteriorCellWindow(1, -2, 4096, 1);
	CHECK(w4k.minCellX == 0);
	CHECK(w4k.maxCellX == 2);
	CHECK(w4k.minCellY == -3);
	CHECK(w4k.maxCellY == -1);

	const auto w8k = SubcellExteriorCellWindow(0, 0, 8192, 0);
	CHECK(w8k.minCellX == 0);
	CHECK(w8k.maxCellX == 1);
	CHECK(w8k.minCellY == 0);
	CHECK(w8k.maxCellY == 1);

	const uint64_t id0 = MakeShapeIdentity(0x00012345u, 0u);
	const uint64_t id1 = MakeShapeIdentity(0x00012345u, 1u);
	const uint64_t id2 = MakeShapeIdentity(0x00012346u, 0u);
	CHECK(id0 != 0u);
	CHECK(id0 != id1);
	CHECK(id0 != id2);
}
