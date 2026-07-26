#pragma once

namespace Util
{
	/** @brief Which shaders a clear-cache action removes. */
	enum class ShaderCacheClearScope
	{
		Full,       ///< Every compiled shader, plus the on-disk cache.
		ActiveOnly  ///< Only the shaders captured drawing the current scene.
	};

	/** @brief Pure scope resolution: the configured default, inverted by the modifier. */
	inline ShaderCacheClearScope ResolveShaderCacheClearScope(bool a_smartDefault, bool a_shiftDown)
	{
		const bool useSmart = a_smartDefault != a_shiftDown;  // XOR: Shift always gives the opposite of the default
		return useSmart ? ShaderCacheClearScope::ActiveOnly : ShaderCacheClearScope::Full;
	}
}
