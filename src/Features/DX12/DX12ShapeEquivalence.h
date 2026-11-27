#pragma once

#include "DX12Resources.h"

#include <cmath>

namespace DX12
{
	[[nodiscard]] inline bool EquivalentGeometry(const Shape& a, const Shape& b)
	{
		if (a.positions.size() != b.positions.size() || a.indices.size() != b.indices.size())
			return false;
		constexpr float kTolerance = 0.01f;
		for (size_t i = 0; i < a.transform.size(); ++i)
			if (std::abs(a.transform[i] - b.transform[i]) > kTolerance)
				return false;
		if (a.indices != b.indices)
			return false;
		for (size_t i = 0; i < a.positions.size(); ++i) {
			const auto& x = a.positions[i];
			const auto& y = b.positions[i];
			if (std::abs(x.x - y.x) > kTolerance ||
				std::abs(x.y - y.y) > kTolerance ||
				std::abs(x.z - y.z) > kTolerance)
				return false;
		}
		return true;
	}
}
