#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

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
	/** @brief Cache exact rigid input bytes, never assume a renderer pointer is a revision. */
	class CasterCoverageCache
	{
	public:
		void Clear()
		{
			entries.clear();
			bytes = 0;
		}

		std::optional<bool> Find(uintptr_t object, const std::array<float, 12>& transform,
			const uint8_t* vertices, const uint16_t* indices, uint32_t stride,
			uint32_t vertexCount, uint32_t triangleCount) const
		{
			const auto it = entries.find(object);
			if (it == entries.end() || !vertices || !indices)
				return std::nullopt;
			const auto& entry = it->second;
			const size_t vertexBytes = size_t(stride) * vertexCount;
			const size_t indexCount = size_t(triangleCount) * 3u;
			if (entry.stride != stride || entry.vertices.size() != vertexBytes ||
				entry.indices.size() != indexCount || entry.transform != transform ||
				std::memcmp(entry.indices.data(), indices, indexCount * sizeof(uint16_t)) != 0 ||
				std::memcmp(entry.vertices.data(), vertices, vertexBytes) != 0)
				return std::nullopt;
			return entry.covered;
		}

		void Store(uintptr_t object, const std::array<float, 12>& transform,
			const uint8_t* vertices, const uint16_t* indices, uint32_t stride,
			uint32_t vertexCount, uint32_t triangleCount, bool covered)
		{
			if (!object || !vertices || !indices || !stride || !vertexCount || !triangleCount)
				return;
			const size_t vertexBytes = size_t(stride) * vertexCount;
			const size_t indexCount = size_t(triangleCount) * 3u;
			const size_t total = vertexBytes + indexCount * sizeof(uint16_t);
			if (auto it = entries.find(object); it != entries.end()) {
				bytes -= it->second.vertices.size() + it->second.indices.size() * sizeof(uint16_t);
				entries.erase(it);
			}
			if (total > kMaxBytes - bytes || entries.size() >= kMaxEntries)
				return;
			Entry entry;
			entry.transform = transform;
			entry.stride = stride;
			entry.covered = covered;
			entry.vertices.assign(vertices, vertices + vertexBytes);
			entry.indices.assign(indices, indices + indexCount);
			entries.emplace(object, std::move(entry));
			bytes += total;
		}

	private:
		static constexpr size_t kMaxBytes = 32u * 1024u * 1024u;
		static constexpr size_t kMaxEntries = 4096u;
		struct Entry
		{
			std::array<float, 12> transform{};
			std::vector<uint8_t> vertices;
			std::vector<uint16_t> indices;
			uint32_t stride = 0;
			bool covered = false;
		};
		std::unordered_map<uintptr_t, Entry> entries;
		size_t bytes = 0;
	};
}
