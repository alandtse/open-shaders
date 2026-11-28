#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace DX12
{
	// Opaque identity values, never dereferenced by a worker. An absent stamp
	// cannot establish ownership; address reuse remains a conservative limitation.
	struct CasterSource
	{
		uintptr_t geometry = 0, vertices = 0, indices = 0;
		uint32_t vertexCount = 0, indexCount = 0, stride = 0, firstIndex = 0;
		constexpr bool operator==(const CasterSource&) const = default;
		[[nodiscard]] constexpr bool Valid() const noexcept
		{
			return geometry && vertices && indices && vertexCount && indexCount && stride && !firstIndex;
		}
	};

	[[nodiscard]] inline bool SameCasterSource(const CasterSource& published, const CasterSource& live,
		const std::array<float, 12>& publishedTransform, const std::array<float, 12>& liveTransform) noexcept
	{
		if (!published.Valid() || !live.Valid() || published != live)
			return false;
		for (std::size_t i = 0; i < publishedTransform.size(); ++i)
			if (!std::isfinite(publishedTransform[i]) || publishedTransform[i] != liveTransform[i])
				return false;
		return true;
	}
}
