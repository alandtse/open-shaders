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

	/** @brief A plugin that is only blocked below a minimum file version. */
	struct OutdatedPlugin
	{
		const wchar_t* dll;           ///< Path relative to the game root.
		REL::Version minimumVersion;  ///< Oldest file version that may coexist with Open Shaders.
		std::string_view reason{};    ///< Shown to the user; empty when the rationale is not recorded.
	};

	/** @brief Version-probed at startup without loading the DLL; any hit disables all hooks and features. */
	inline constexpr OutdatedPlugin outdatedPlugins[] = {
		{ L"Data/SKSE/Plugins/SexLabUtil.dll", REL::Version(2, 0, 0, 0), "use SexLab P+ instead" }
	};
}
