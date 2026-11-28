#pragma once

#include <cstdint>

namespace DX12
{
	/** @brief Whether a traversal passed its scene-graph depth limit and must report the subtree as missing. */
	[[nodiscard]] constexpr bool IsSceneGraphDepthExceeded(uint32_t a_depth, uint32_t a_maxDepth) noexcept
	{
		return a_depth > a_maxDepth;
	}

	/** @brief Reports whether a reference's placement can change independently of its static record. */
	[[nodiscard]] constexpr bool IsRuntimeDependentPlacement(
		bool a_movableStatic, bool a_hasScripts, bool a_hasEnableParent, bool a_initiallyDisabled) noexcept
	{
		return a_movableStatic || a_hasScripts || a_hasEnableParent || a_initiallyDisabled;
	}
}
