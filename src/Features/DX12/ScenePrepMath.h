#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DX12
{
	inline constexpr uint64_t kVertexPositionFlagBit = 1ull << 44;

	[[nodiscard]] constexpr bool HasVertexPositionStream(uint64_t a_rawVertexDesc) noexcept
	{
		return (a_rawVertexDesc & kVertexPositionFlagBit) != 0;
	}

	[[nodiscard]] constexpr uint32_t VertexStrideBytes(uint64_t a_rawVertexDesc) noexcept
	{
		return static_cast<uint32_t>(a_rawVertexDesc & 0xFull) * sizeof(float);
	}

	/** @brief Resolves the index slice {firstIndex, indexCount} for a BSMeshLODTriShape. */
	[[nodiscard]] inline std::optional<std::pair<uint32_t, uint32_t>> ResolveMeshLODSlice(
		uint8_t a_lodMode, std::span<const uint32_t, 3> a_lodTriangleCounts, uint32_t a_totalTriangles) noexcept
	{
		constexpr uint8_t kSingleLevel = 0x80u;
		if ((a_lodMode & kSingleLevel) != 0) {
			if (a_lodTriangleCounts[0] > 0 && a_lodTriangleCounts[0] <= a_totalTriangles)
				return std::pair{ 0u, a_lodTriangleCounts[0] * 3u };
			const uint32_t level = static_cast<uint32_t>(a_lodMode & ~kSingleLevel);
			if (level >= 3u)
				return std::nullopt;
			uint64_t prefix = 0;
			for (uint32_t i = 0; i < level; ++i)
				prefix += uint64_t(a_lodTriangleCounts[i]) * 3u;
			const uint64_t count = uint64_t(a_lodTriangleCounts[level]) * 3u;
			if (count == 0 || prefix + count > uint64_t(a_totalTriangles) * 3u)
				return std::nullopt;
			return std::pair{ static_cast<uint32_t>(prefix), static_cast<uint32_t>(count) };
		}
		const uint64_t sum = uint64_t(a_lodTriangleCounts[0]) + a_lodTriangleCounts[1] + a_lodTriangleCounts[2];
		const uint64_t tris = (sum > 0 && sum <= a_totalTriangles) ? sum : uint64_t(a_totalTriangles);
		if (tris == 0 || tris > (std::numeric_limits<uint32_t>::max() / 3u))
			return std::nullopt;
		return std::pair{ 0u, static_cast<uint32_t>(tris * 3u) };
	}

	/** @brief Decodes interleaved float3 positions and u16 triangle indices with bounds and finiteness checks. */
	[[nodiscard]] inline bool DecodeRawMeshBuffers(
		const uint8_t* a_rawVertices, uint32_t a_vertexCount, uint32_t a_stride,
		const uint16_t* a_rawIndices, uint32_t a_firstIndex, uint32_t a_indexCount,
		std::vector<float3>& a_outPositions, std::vector<uint32_t>& a_outIndices)
	{
		a_outPositions.clear();
		a_outIndices.clear();
		if (!a_rawVertices || !a_rawIndices || a_stride < 16 || a_stride > 256 ||
			a_vertexCount == 0 || a_vertexCount > 65535u ||
			a_indexCount < 3 || (a_indexCount % 3u) != 0 || a_indexCount > 65535u * 3u)
			return false;

		a_outPositions.reserve(a_vertexCount);
		for (uint32_t v = 0; v < a_vertexCount; ++v) {
			float xyz[3];
			std::memcpy(xyz, a_rawVertices + size_t(v) * a_stride, sizeof(xyz));
			if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
				a_outPositions.clear();
				return false;
			}
			a_outPositions.push_back(float3{ xyz[0], xyz[1], xyz[2] });
		}

		a_outIndices.reserve(a_indexCount);
		for (uint32_t i = 0; i < a_indexCount; ++i) {
			const uint16_t idx = a_rawIndices[size_t(a_firstIndex) + i];
			if (idx >= a_vertexCount) {
				a_outPositions.clear();
				a_outIndices.clear();
				return false;
			}
			a_outIndices.push_back(idx);
		}
		return true;
	}

	struct AffineTransform
	{
		std::array<std::array<float, 3>, 3> rotate{ { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } } };
		float3 translate{};
		float scale = 1.0f;
	};

	/** @brief Builds an affine transform matching RE::NiMatrix3::SetEulerAnglesXYZ placement order. */
	[[nodiscard]] inline std::optional<AffineTransform> PlacementFromEulerXYZ(
		const float3& a_position, const float3& a_rotation, float a_scale) noexcept
	{
		if (!std::isfinite(a_position.x) || !std::isfinite(a_position.y) || !std::isfinite(a_position.z) ||
			!std::isfinite(a_rotation.x) || !std::isfinite(a_rotation.y) || !std::isfinite(a_rotation.z) ||
			!std::isfinite(a_scale) || a_scale <= 0.0f)
			return std::nullopt;

		const float sx = std::sin(a_rotation.x), cx = std::cos(a_rotation.x);
		const float sy = std::sin(a_rotation.y), cy = std::cos(a_rotation.y);
		const float sz = std::sin(a_rotation.z), cz = std::cos(a_rotation.z);

		AffineTransform result;
		result.rotate[0] = { cy * cz, cy * sz, -sy };
		result.rotate[1] = { sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy };
		result.rotate[2] = { cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy };
		result.translate = a_position;
		result.scale = a_scale;
		return result;
	}

	[[nodiscard]] inline AffineTransform ComposeAffine(
		const AffineTransform& a_parent, const AffineTransform& a_local) noexcept
	{
		AffineTransform result;
		result.scale = a_parent.scale * a_local.scale;
		for (size_t r = 0; r < 3; ++r) {
			for (size_t c = 0; c < 3; ++c) {
				result.rotate[r][c] = a_parent.rotate[r][0] * a_local.rotate[0][c] +
				                      a_parent.rotate[r][1] * a_local.rotate[1][c] +
				                      a_parent.rotate[r][2] * a_local.rotate[2][c];
			}
		}
		const float lx = a_local.translate.x, ly = a_local.translate.y, lz = a_local.translate.z;
		result.translate = float3{
			a_parent.translate.x + (a_parent.rotate[0][0] * lx + a_parent.rotate[0][1] * ly + a_parent.rotate[0][2] * lz) * a_parent.scale,
			a_parent.translate.y + (a_parent.rotate[1][0] * lx + a_parent.rotate[1][1] * ly + a_parent.rotate[1][2] * lz) * a_parent.scale,
			a_parent.translate.z + (a_parent.rotate[2][0] * lx + a_parent.rotate[2][1] * ly + a_parent.rotate[2][2] * lz) * a_parent.scale
		};
		return result;
	}

	[[nodiscard]] inline std::array<float, 12> TransformToRows3x4(const AffineTransform& a_transform) noexcept
	{
		std::array<float, 12> out{};
		for (size_t r = 0; r < 3; ++r)
			for (size_t c = 0; c < 3; ++c)
				out[4 * r + c] = a_transform.rotate[r][c] * a_transform.scale;
		out[3] = a_transform.translate.x;
		out[7] = a_transform.translate.y;
		out[11] = a_transform.translate.z;
		return out;
	}

	[[nodiscard]] inline bool IsInvertibleTransform3x4(const std::array<float, 12>& a_rows) noexcept
	{
		for (float value : a_rows)
			if (!std::isfinite(value))
				return false;
		const float c00 = a_rows[5] * a_rows[10] - a_rows[6] * a_rows[9];
		const float c01 = a_rows[4] * a_rows[10] - a_rows[6] * a_rows[8];
		const float c02 = a_rows[4] * a_rows[9] - a_rows[5] * a_rows[8];
		const float det = a_rows[0] * c00 - a_rows[1] * c01 + a_rows[2] * c02;
		return std::isfinite(det) && std::abs(det) > 1e-8f;
	}

	[[nodiscard]] inline bool InsideSubcellXY(
		double a_x, double a_y, int64_t a_tileX, int64_t a_tileY, uint32_t a_size) noexcept
	{
		if (!std::isfinite(a_x) || !std::isfinite(a_y) || !a_size)
			return false;
		const double s = double(a_size);
		return a_x >= double(a_tileX) * s && a_x < double(a_tileX + 1) * s &&
		       a_y >= double(a_tileY) * s && a_y < double(a_tileY + 1) * s;
	}

	[[nodiscard]] inline bool InCandidateHaloXY(
		double a_x, double a_y, int64_t a_tileX, int64_t a_tileY, uint32_t a_size, int64_t a_haloTiles = 2) noexcept
	{
		if (!std::isfinite(a_x) || !std::isfinite(a_y) || !a_size)
			return false;
		const double s = double(a_size);
		return a_x >= double(a_tileX - a_haloTiles) * s && a_x < double(a_tileX + a_haloTiles + 1) * s &&
		       a_y >= double(a_tileY - a_haloTiles) * s && a_y < double(a_tileY + a_haloTiles + 1) * s;
	}

	/** @brief Returns nullopt if geometry/transform XY is empty or non-finite; otherwise whether AABB overlaps the subcell. */
	[[nodiscard]] inline std::optional<bool> OverlapsSubcellXY(
		std::span<const float3> a_positions, const std::array<float, 12>& a_rows,
		int64_t a_tileX, int64_t a_tileY, uint32_t a_size) noexcept
	{
		if (a_positions.empty() || !a_size)
			return std::nullopt;
		double lowX = std::numeric_limits<double>::infinity();
		double lowY = std::numeric_limits<double>::infinity();
		double highX = -std::numeric_limits<double>::infinity();
		double highY = -std::numeric_limits<double>::infinity();
		for (const auto& p : a_positions) {
			const double wx = double(a_rows[0]) * p.x + double(a_rows[1]) * p.y + double(a_rows[2]) * p.z + double(a_rows[3]);
			const double wy = double(a_rows[4]) * p.x + double(a_rows[5]) * p.y + double(a_rows[6]) * p.z + double(a_rows[7]);
			if (!std::isfinite(wx) || !std::isfinite(wy))
				return std::nullopt;
			lowX = std::min(lowX, wx);
			lowY = std::min(lowY, wy);
			highX = std::max(highX, wx);
			highY = std::max(highY, wy);
		}
		const double s = double(a_size);
		return lowX < double(a_tileX + 1) * s && highX >= double(a_tileX) * s &&
		       lowY < double(a_tileY + 1) * s && highY >= double(a_tileY) * s;
	}

	/** @brief Planar extent of placed references, used to decide whole-cell residency. */
	struct PlacementBounds
	{
		double minX = std::numeric_limits<double>::infinity(), minY = std::numeric_limits<double>::infinity();
		double maxX = -std::numeric_limits<double>::infinity(), maxY = -std::numeric_limits<double>::infinity();

		void Include(double a_x, double a_y) noexcept
		{
			if (!std::isfinite(a_x) || !std::isfinite(a_y))
				return;
			minX = std::min(minX, a_x);
			minY = std::min(minY, a_y);
			maxX = std::max(maxX, a_x);
			maxY = std::max(maxY, a_y);
		}
		[[nodiscard]] bool Valid() const noexcept { return minX <= maxX && minY <= maxY; }
	};

	/** @brief True when the bounds fit a (2r+1)-subcell square centred on their own centre subcell. */
	[[nodiscard]] inline bool FitsSubcellFootprint(const PlacementBounds& a_bounds, uint32_t a_size, int a_radius) noexcept
	{
		if (!a_bounds.Valid() || !a_size || a_radius < 0)
			return false;
		const double s = double(a_size);
		const double cx = std::floor((a_bounds.minX + a_bounds.maxX) / (2.0 * s));
		const double cy = std::floor((a_bounds.minY + a_bounds.maxY) / (2.0 * s));
		return a_bounds.minX >= (cx - a_radius) * s && a_bounds.minY >= (cy - a_radius) * s &&
		       a_bounds.maxX < (cx + a_radius + 1) * s && a_bounds.maxY < (cy + a_radius + 1) * s;
	}

	struct ExteriorCellWindow
	{
		int32_t minCellX = 0, maxCellX = 0, minCellY = 0, maxCellY = 0;
	};

	[[nodiscard]] inline ExteriorCellWindow SubcellExteriorCellWindow(
		int64_t a_tileX, int64_t a_tileY, uint32_t a_size, int32_t a_haloCells = 0) noexcept
	{
		constexpr double kExteriorCellUnits = 4096.0;
		const double s = double(a_size);
		return {
			int32_t(std::floor((double(a_tileX) * s) / kExteriorCellUnits)) - a_haloCells,
			int32_t(std::floor((double(a_tileX + 1) * s - 1.0) / kExteriorCellUnits)) + a_haloCells,
			int32_t(std::floor((double(a_tileY) * s) / kExteriorCellUnits)) - a_haloCells,
			int32_t(std::floor((double(a_tileY + 1) * s - 1.0) / kExteriorCellUnits)) + a_haloCells
		};
	}

	[[nodiscard]] inline std::optional<std::string> NormalizeNifPath(std::string_view a_rawPath)
	{
		auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
		while (!a_rawPath.empty() && isSpace(static_cast<unsigned char>(a_rawPath.front())))
			a_rawPath.remove_prefix(1);
		while (!a_rawPath.empty() && isSpace(static_cast<unsigned char>(a_rawPath.back())))
			a_rawPath.remove_suffix(1);
		if (a_rawPath.empty() || a_rawPath.size() > 260)
			return std::nullopt;

		std::string normalized;
		normalized.reserve(a_rawPath.size() + 7);
		for (char ch : a_rawPath) {
			const unsigned char uch = static_cast<unsigned char>(ch);
			if (uch < 0x20 || ch == ':')
				return std::nullopt;
			normalized.push_back(ch == '/' ? '\\' : ch);
		}
		if (normalized.front() == '\\' || normalized.back() == '\\')
			return std::nullopt;

		size_t start = 0;
		while (start < normalized.size()) {
			const size_t sep = normalized.find('\\', start);
			const std::string_view segment = sep == std::string::npos ?
			                                     std::string_view(normalized).substr(start) :
			                                     std::string_view(normalized).substr(start, sep - start);
			if (segment.empty() || segment == "." || segment == "..")
				return std::nullopt;
			if (sep == std::string::npos)
				break;
			start = sep + 1;
		}

		if (normalized.size() < 5)
			return std::nullopt;
		const std::string_view ext = std::string_view(normalized).substr(normalized.size() - 4);
		if (std::tolower(static_cast<unsigned char>(ext[0])) != '.' ||
			std::tolower(static_cast<unsigned char>(ext[1])) != 'n' ||
			std::tolower(static_cast<unsigned char>(ext[2])) != 'i' ||
			std::tolower(static_cast<unsigned char>(ext[3])) != 'f')
			return std::nullopt;

		constexpr std::string_view kMeshesPrefix = "meshes\\";
		if (normalized.size() >= kMeshesPrefix.size()) {
			bool hasPrefix = true;
			for (size_t i = 0; i < kMeshesPrefix.size(); ++i) {
				if (std::tolower(static_cast<unsigned char>(normalized[i])) != kMeshesPrefix[i]) {
					hasPrefix = false;
					break;
				}
			}
			if (hasPrefix)
				normalized.replace(0, kMeshesPrefix.size(), kMeshesPrefix);
			else
				normalized.insert(0, kMeshesPrefix);
		} else {
			normalized.insert(0, kMeshesPrefix);
		}

		if (normalized.size() > 260)
			return std::nullopt;
		return normalized;
	}

	[[nodiscard]] constexpr uint64_t MakeShapeIdentity(uint32_t a_ownerFormID, uint32_t a_shapeOrdinal) noexcept
	{
		uint64_t hash = 14695981039346656037ull;
		constexpr uint64_t kPrime = 1099511628211ull;
		for (unsigned i = 0; i < 4; ++i) {
			hash ^= static_cast<uint8_t>((a_ownerFormID >> (i * 8)) & 0xFFu);
			hash *= kPrime;
		}
		hash ^= 0xFFu;
		hash *= kPrime;
		for (unsigned i = 0; i < 4; ++i) {
			hash ^= static_cast<uint8_t>((a_shapeOrdinal >> (i * 8)) & 0xFFu);
			hash *= kPrime;
		}
		return hash ? hash : 1;
	}
}
