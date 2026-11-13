#pragma once

namespace DX12Native
{
	/** @brief Keeps prepared geometry independent from texture-manifest policy. */
	inline bool UsePreparedMaterialManifest(bool a_runtimeOnlyMaterials, bool a_hasResidentGeneration)
	{
		return !a_runtimeOnlyMaterials && !a_hasResidentGeneration;
	}
}
