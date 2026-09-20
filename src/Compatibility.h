#pragma once

/**
 * @file Compatibility.h
 * @brief Plugins that cannot run alongside Open Shaders.
 *
 * Upstream keeps this list inline in XSEPlugin.cpp. An upstream sync that adds
 * an entry there must translate it into the table below, or the block silently
 * stops applying.
 */
namespace Compatibility
{
	/** @brief A blocked plugin and, where known, why it is blocked. */
	struct IncompatiblePlugin
	{
		const wchar_t* dll;         ///< Path relative to the game root.
		std::string_view reason{};  ///< Shown to the user; empty when the rationale is not recorded.
	};

	/** @brief Probed with LoadLibrary at startup; any hit disables all hooks and features. */
	inline constexpr IncompatiblePlugin incompatiblePlugins[] = {
		{ L"Data/SKSE/Plugins/ShaderTools.dll" },
		{ L"Data/SKSE/Plugins/SSEShaderTools.dll" },
		{ L"Data/SKSE/Plugins/SkyrimUpscaler.dll" },
		{ L"Data/SKSE/Plugins/EVLaS.dll", "superseded by Sky Sync" },
		{ L"Data/SKSE/Plugins/AELAS.dll", "superseded by Sky Sync" },
		{ L"Data/SKSE/Plugins/SSEReShadeHelper.dll" },
		{ L"Data/SKSE/Plugins/TAASharpen.dll" },
		{ L"Data/SKSE/Plugins/NVIDIA_Reflex.dll" },
		{ L"Data/SKSE/Plugins/MARA.dll" },
		{ L"Data/SKSE/Plugins/NativeWaterLightStabilizer.dll",
			"superseded by Sky Reflection and Light Limit Fix" },
		{ L"Data/SKSE/Plugins/intellightent-ng.dll" },
		{ L"Data/SKSE/Plugins/DynamicWetness.dll" }
	};
}
