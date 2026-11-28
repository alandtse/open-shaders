#pragma once

namespace DX12
{
	/** @brief Reports whether a reference's placement can change independently of its static record. */
	[[nodiscard]] constexpr bool IsRuntimeDependentPlacement(
		bool a_movableStatic, bool a_hasScripts, bool a_hasEnableParent, bool a_initiallyDisabled) noexcept
	{
		return a_movableStatic || a_hasScripts || a_hasEnableParent || a_initiallyDisabled;
	}
}
