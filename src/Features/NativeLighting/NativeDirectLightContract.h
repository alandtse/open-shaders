#pragma once

#include <array>
#include <cstdint>

// Matches the existing NativeLighting/Direct.hlsl6 PointLight structured-buffer ABI.
// This is unshaded source data, not a diffuse/specular output.
namespace DX12Native
{
	struct NativeDirectPointLight
	{
		float4 positionRadius{};
		float4 radianceSourceRadius{};
		std::array<uint32_t, 4> flags{};
	};
	static_assert(sizeof(NativeDirectPointLight) == 48);
}
