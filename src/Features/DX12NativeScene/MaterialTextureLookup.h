#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace DX12Native
{
	/** @brief Canonical lookup key shared by atlas and loose DDS publishers and material records. */
	inline std::string MaterialTextureLookupKey(std::string a_path)
	{
		std::ranges::transform(a_path, a_path.begin(), [](unsigned char c) {
			return c == '/' ? '\\' : char(std::tolower(c));
		});
		if (!a_path.empty() && !a_path.starts_with("textures\\"))
			a_path = "textures\\" + a_path;
		return a_path;
	}
}
