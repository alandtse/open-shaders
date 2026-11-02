#pragma once

#include "VRAPI/TerrainDepthOffset.h"

#include <cmath>
#include <d3d11.h>

namespace Util::TerrainDepth
{
	inline constexpr float StandardOffset = 5.0f;
	inline constexpr GUID MetadataId{ 0x5136c6bd, 0x4909, 0x4bf8, { 0x9d, 0x92, 0xd4, 0x0a, 0x56, 0xe3, 0x7a, 0x35 } };

	/** @brief Attaches the compiled offset contract to the shader's COM lifetime. */
	inline bool Publish(ID3D11VertexShader* shader, const CSPluginAPI::TerrainDepthOffset& offset)
	{
		return shader && std::isfinite(offset.standardOffset) && offset.reverseZ <= 1 &&
		       SUCCEEDED(shader->SetPrivateData(MetadataId, sizeof(offset), &offset));
	}

	/** @brief Reads shader metadata without compiling, retaining, or querying device context state. */
	inline bool Query(ID3D11VertexShader* shader, CSPluginAPI::TerrainDepthOffset* offset)
	{
		if (!offset)
			return false;
		*offset = {};
		CSPluginAPI::TerrainDepthOffset value{};
		UINT size = sizeof(value);
		if (!shader || FAILED(shader->GetPrivateData(MetadataId, &size, &value)) ||
			size != sizeof(value) || !std::isfinite(value.standardOffset) || value.reverseZ > 1)
			return false;
		*offset = value;
		return true;
	}
}
