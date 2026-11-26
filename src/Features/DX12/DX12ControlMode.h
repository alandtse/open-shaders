#pragma once

#include <algorithm>

namespace DX12
{
	// Persisted mode IDs; keep these stable independently of dropdown order.
	enum class ControlMode : int
	{
		ShadowOffload = 0,
		RasterComparison = 1,
		FullRT = 2
	};

	constexpr ControlMode SanitizeControlMode(int mode, bool developerMode)
	{
		const auto selected = static_cast<ControlMode>(std::clamp(mode,
			static_cast<int>(ControlMode::ShadowOffload), static_cast<int>(ControlMode::FullRT)));
		return selected == ControlMode::RasterComparison && !developerMode ? ControlMode::ShadowOffload : selected;
	}
}
