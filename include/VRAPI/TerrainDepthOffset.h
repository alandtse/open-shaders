// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace CSPluginAPI
{
	/** @brief Revision-5 terrain offset before perspective division; reverseZ follows CameraProj. */
	struct TerrainDepthOffset
	{
		float standardOffset{};
		uint32_t reverseZ{};
	};
	static_assert(sizeof(TerrainDepthOffset) == 8);

	inline constexpr uint32_t TerrainDepthSupportMessageType = 0x43535444;  // "CSTD"
	/** @brief Synchronous PostPostLoad query; the consumer acknowledges only after acquiring revision 5. */
	struct TerrainDepthSupportMessage
	{
		uint32_t revision{ 5 };
		uint32_t supported{};
	};
}
