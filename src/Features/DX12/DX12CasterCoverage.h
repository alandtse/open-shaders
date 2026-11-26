#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DX12Native
{
	inline constexpr float kCasterCoverageTolerance = 0.01f;

	inline uint64_t CasterCoverageKey(size_t vertices, size_t indices)
	{
		if (!vertices || !indices || vertices > UINT32_MAX || indices > UINT32_MAX)
			return 0;
		return (uint64_t(vertices) << 32) | uint64_t(indices);
	}

	inline bool CasterCoverageEqual(float a, float b)
	{
		return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= kCasterCoverageTolerance;
	}

	// Match actual rigid triangles and pose, not a name, reference or tile.
	template <class Shape>
	bool MatchesCasterGeometry(const Shape& shape, const std::array<float, 12>& transform,
		const uint8_t* vertices, const uint16_t* indices, uint32_t stride, uint32_t vertexCount, uint32_t triangleCount)
	{
		const uint64_t indexCount = uint64_t(triangleCount) * 3u;
		if (!vertices || !indices || stride < sizeof(float) * 3 || !vertexCount || !triangleCount ||
			shape.positions.size() != vertexCount || shape.indices.size() != indexCount)
			return false;
		for (size_t i = 0; i < transform.size(); ++i)
			if (!CasterCoverageEqual(shape.transform[i], transform[i]))
				return false;
		for (size_t i = 0; i < indexCount; ++i)
			if (indices[i] >= vertexCount || shape.indices[i] != indices[i])
				return false;
		for (size_t i = 0; i < vertexCount; ++i) {
			float xyz[3];
			std::memcpy(xyz, vertices + i * stride, sizeof(xyz));
			const auto& position = shape.positions[i];
			if (!CasterCoverageEqual(position.x, xyz[0]) || !CasterCoverageEqual(position.y, xyz[1]) ||
				!CasterCoverageEqual(position.z, xyz[2]))
				return false;
		}
		return true;
	}
}
