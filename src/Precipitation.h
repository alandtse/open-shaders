#pragma once

namespace Precipitation
{
	/** @brief Installs the shared precipitation render hook. */
	void Install();

	/** @brief Invokes the engine precipitation render path captured by the shared hook. */
	void RenderOriginal();
}
