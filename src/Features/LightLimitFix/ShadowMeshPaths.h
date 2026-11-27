#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace ShadowMeshExclusions
{
	using MeshPaths = std::set<std::string>;

	/** @brief Canonical mesh-relative NIF path used by both JSON files and the picker. */
	inline std::string MeshPath(std::string path)
	{
		for (char& c : path) {
			if (static_cast<unsigned char>(c) < ' ')
				return {};
			if (c == '\\')
				c = '/';
			else if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
		}
		for (const std::string_view prefix : { "data/meshes/", "meshes/" }) {
			if (path.starts_with(prefix)) {
				path.erase(0, prefix.size());
				break;
			}
		}
		if (!path.ends_with(".nif") || path.find_first_of(":*?\"<>|") != path.npos)
			return {};
		for (std::string_view rest = path;;) {
			const auto slash = rest.find('/');
			const auto component = rest.substr(0, slash);
			if (component.empty() || component == "." || component == ".." || component == ".nif")
				return {};
			if (slash == rest.npos)
				break;
			rest.remove_prefix(slash + 1);
		}
		return path;
	}

	/** @brief Reads one exclusion file as a unit, preserving a malformed file for its author. */
	inline std::optional<MeshPaths> ReadMeshes(const nlohmann::json& document)
	{
		const auto meshes = document.find("meshes");
		if (meshes == document.end() || !meshes->is_array())
			return std::nullopt;
		MeshPaths result;
		for (const auto& mesh : *meshes) {
			if (!mesh.is_string())
				return std::nullopt;
			auto path = MeshPath(mesh.get<std::string>());
			if (path.empty())
				return std::nullopt;
			result.insert(std::move(path));
		}
		return result;
	}
}
