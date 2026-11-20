#include "Features/DX12NativeScene/PreparedSnowProjection.h"
#include "Features/NativePBR/GpuMaterialAbi.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace
{
	nlohmann::json SnowRow()
	{
		return {
			{ "projectedSnow", {
				{ "basis", "world-up-face-projection-v1" }, { "candidate", true },
				{ "projectedArea", 2.5 }, { "surfaceArea", 4.0 },
				{ "upwardTriangles", 1 }, { "totalTriangles", 2 }
			} }
		};
	}
}

TEST_CASE("Projected snow metadata admits only prepared upward opaque geometry", "[dx12_resources][snow]")
{
	const auto row = SnowRow();
	CHECK(DX12Native::ReadPreparedSnowProjection(row, 6, true));
	CHECK_FALSE(DX12Native::ReadPreparedSnowProjection(row, 6, false));
	CHECK_FALSE(DX12Native::ReadPreparedSnowProjection(nlohmann::json::object(), 6, true));

	auto noProjection = row;
	noProjection["projectedSnow"]["candidate"] = false;
	noProjection["projectedSnow"]["projectedArea"] = 0.0;
	noProjection["projectedSnow"]["upwardTriangles"] = 0;
	CHECK_FALSE(DX12Native::ReadPreparedSnowProjection(noProjection, 6, true));

	auto unavailable = row;
	unavailable["projectedSnow"] = {
		{ "basis", "unavailable:geometry-io" }, { "candidate", false },
		{ "projectedArea", 0.0 }, { "surfaceArea", 0.0 },
		{ "upwardTriangles", 0 }, { "totalTriangles", 0 }
	};
	CHECK_FALSE(DX12Native::ReadPreparedSnowProjection(unavailable, 6, true));
}

TEST_CASE("Projected snow rejects inconsistent or unsupported metadata", "[dx12_resources][snow]")
{
	auto row = SnowRow();
	SECTION("triangle index range")
	{
		CHECK_THROWS(DX12Native::ReadPreparedSnowProjection(row, 3, true));
	}
	SECTION("false candidate despite upward area")
	{
		row["projectedSnow"]["candidate"] = false;
		CHECK_THROWS(DX12Native::ReadPreparedSnowProjection(row, 6, true));
	}
	SECTION("upward triangles exceed total")
	{
		row["projectedSnow"]["upwardTriangles"] = 3;
		CHECK_THROWS(DX12Native::ReadPreparedSnowProjection(row, 6, true));
	}
	SECTION("impossible projected footprint")
	{
		row["projectedSnow"]["projectedArea"] = 5.0;
		CHECK_THROWS(DX12Native::ReadPreparedSnowProjection(row, 6, true));
	}
	SECTION("non-finite areas")
	{
		row["projectedSnow"]["surfaceArea"] = std::numeric_limits<double>::infinity();
		CHECK_THROWS(DX12Native::ReadPreparedSnowProjection(row, 6, true));
	}
	SECTION("unknown projection basis")
	{
		row["projectedSnow"]["basis"] = "texture-based";
		CHECK_THROWS(DX12Native::ReadPreparedSnowProjection(row, 6, true));
	}
	SECTION("missing metadata field")
	{
		row["projectedSnow"].erase("surfaceArea");
		CHECK_THROWS(DX12Native::ReadPreparedSnowProjection(row, 6, true));
	}
}

TEST_CASE("NativePBR snow-candidate flag shares existing compact draw binding", "[NativePBR][snow]")
{
	const auto draw = NativePBR::PackGpuDrawBinding(1, 2, 3,
		NativePBR::kGpuDrawPreparedOpaque | NativePBR::kGpuDrawProjectedSnowCandidate);
	CHECK(draw.flags == 12u);
	CHECK(sizeof(NativePBR::GpuDrawBinding) == 24);
}
