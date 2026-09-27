#include "MeshRules.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace TerrainVariationTextures
{
	namespace
	{
		constexpr std::string_view LandscapeDirectory = "landscape/";
	}

	std::string CanonicaliseTexturePath(std::string_view a_path)
	{
		std::string canonical(a_path);
		for (auto& character : canonical) {
			character = character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}

		if (canonical.starts_with("data/")) {
			canonical.erase(0, 5);
		}
		if (canonical.starts_with("textures/")) {
			canonical.erase(0, 9);
		}

		return canonical;
	}

	void MeshRules::LoadFile(const std::filesystem::path& a_path)
	{
		std::ifstream stream(a_path);
		if (!stream) {
			throw std::runtime_error("Unable to open rules file");
		}
		const auto document = nlohmann::json::parse(stream);
		if (!document.is_object() || document.empty()) {
			throw std::runtime_error("Expected an object containing include and/or exclude objects");
		}

		MeshRules fileRules;
		for (const auto& [action, patterns] : document.items()) {
			auto* rules = action == "include" ? &fileRules.included : action == "exclude" ? &fileRules.excluded :
			                                                                                nullptr;
			if (rules == nullptr || !patterns.is_object()) {
				throw std::runtime_error("Rules must be include or exclude objects");
			}
			for (const auto& [kind, entries] : patterns.items()) {
				auto* destination = kind == "filenames" ? &rules->filenames : kind == "paths"   ? &rules->paths :
				                                                          kind == "directories" ? &rules->directories :
				                                                                                  nullptr;
				if (destination == nullptr || !entries.is_array()) {
					throw std::runtime_error("Patterns must be filenames, paths, or directories arrays");
				}
				for (const auto& entry : entries) {
					auto path = CanonicaliseTexturePath(entry.get<std::string>());
					if (path.empty() || path.front() == '/' || path.find_first_of(":*?") != std::string::npos || path.find('\0') != std::string::npos ||
						path == ".." || path.starts_with("../") || path.ends_with("/..") || path.find("/../") != std::string::npos) {
						throw std::runtime_error("Patterns require nonempty relative texture paths without wildcards or parent traversal");
					}
					if (kind == "filenames" && path.find('/') != std::string::npos) {
						throw std::runtime_error("Filename patterns must not contain directories");
					}
					if (kind == "directories") {
						if (!path.ends_with('/')) {
							path += '/';
						}
					} else if (!path.ends_with(".dds")) {
						throw std::runtime_error("Filename and path patterns must end with .dds");
					}
					destination->insert(std::move(path));
				}
			}
		}
		included.Merge(fileRules.included);
		excluded.Merge(fileRules.excluded);
	}

	MeshRules::LoadResult MeshRules::Load(const std::filesystem::path& a_directory)
	{
		included = {};
		excluded = {};
		LoadResult result;
		std::error_code error;
		if (!std::filesystem::is_directory(a_directory, error)) {
			if (error && error != std::errc::no_such_file_or_directory) {
				result.errors.push_back(a_directory.string() + ": " + error.message());
			}
			return result;
		}

		for (std::filesystem::recursive_directory_iterator it(a_directory, error), end; !error && it != end; it.increment(error)) {
			std::error_code entryError;
			if (!it->is_regular_file(entryError)) {
				if (entryError) {
					result.errors.push_back(it->path().string() + ": " + entryError.message());
				}
				continue;
			}
			if (CanonicaliseTexturePath(it->path().extension().string()) != ".json") {
				continue;
			}
			try {
				LoadFile(it->path());
				++result.loadedFiles;
			} catch (const std::exception& exception) {
				result.errors.push_back(it->path().string() + ": " + exception.what());
			}
		}
		if (error) {
			result.errors.push_back(a_directory.string() + ": " + error.message());
		}
		return result;
	}

	bool MeshRules::TexturePatterns::Matches(std::string_view a_canonicalPath) const
	{
		const auto filename = a_canonicalPath.substr(a_canonicalPath.find_last_of('/') + 1);
		return filenames.contains(std::string(filename)) || paths.contains(std::string(a_canonicalPath)) ||
		       std::ranges::any_of(directories, [a_canonicalPath](const auto& directory) { return a_canonicalPath.starts_with(directory); });
	}

	void MeshRules::TexturePatterns::Merge(TexturePatterns& a_other)
	{
		filenames.merge(a_other.filenames);
		paths.merge(a_other.paths);
		directories.merge(a_other.directories);
	}

	bool MeshRules::IsExcluded(std::string_view a_canonicalPath) const
	{
		return excluded.Matches(a_canonicalPath);
	}

	bool MeshRules::IsEligible(std::string_view a_canonicalPath, bool a_isLandscapeTexture) const
	{
		const bool isDirectLandscapeTexture = a_canonicalPath.starts_with(LandscapeDirectory) &&
		                                      a_canonicalPath.find('/', LandscapeDirectory.size()) == std::string_view::npos;
		return !IsExcluded(a_canonicalPath) && (isDirectLandscapeTexture || a_isLandscapeTexture || included.Matches(a_canonicalPath));
	}
}
