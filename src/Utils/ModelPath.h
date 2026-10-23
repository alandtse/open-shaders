#pragma once

#include "Utils/Format.h"

namespace Util
{
	inline constexpr std::size_t kMaximumModelPathLength = 260;

	/** @brief Normalizes a source model path to a lowercase meshes-relative path. */
	inline std::string NormalizeModelPath(std::string a_path)
	{
		a_path = FixFilePath(a_path);
		while (a_path.starts_with("./"))
			a_path.erase(0, 2);
		if (const auto meshesPosition = a_path.find("meshes/"); meshesPosition != std::string::npos)
			a_path.erase(0, meshesPosition);
		else
			a_path.insert(0, "meshes/");
		return a_path;
	}

	/** @brief Accepts a bounded meshes-relative NIF path without parent traversal or drive prefixes. */
	inline bool IsSafeModelPath(const std::string& a_path)
	{
		return a_path.size() <= kMaximumModelPathLength && a_path.starts_with("meshes/") &&
		       a_path.ends_with(".nif") && a_path.find("../") == std::string::npos &&
		       a_path.find(':') == std::string::npos;
	}
}
