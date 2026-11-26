#pragma once

#include "ScenePrepMath.h"

namespace DX12
{
	[[nodiscard]] inline AffineTransform FromNiTransform(const RE::NiTransform& a_source) noexcept
	{
		AffineTransform result;
		for (size_t r = 0; r < 3; ++r)
			for (size_t c = 0; c < 3; ++c)
				result.rotate[r][c] = a_source.rotate.entry[r][c];
		result.translate = float3{ a_source.translate.x, a_source.translate.y, a_source.translate.z };
		result.scale = a_source.scale;
		return result;
	}
}
