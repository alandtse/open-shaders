#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace TerrainVariationTextures
{
	/** @brief Normalizes texture names to lowercase paths relative to the textures directory. */
	std::string CanonicaliseTexturePath(std::string_view a_path);

	class MeshRules
	{
	public:
		struct LoadResult
		{
			std::size_t loadedFiles = 0;
			std::vector<std::string> errors;
		};

		/** @brief Replaces rules with the union of valid JSON files in the directory and its subdirectories. */
		LoadResult Load(const std::filesystem::path& a_directory);
		/** @brief Tests a canonical texture path against the loaded exclusions. */
		bool IsExcluded(std::string_view a_canonicalPath) const;
		/** @brief Combines automatic and whitelist matching, with blacklist matches taking precedence. */
		bool IsEligible(std::string_view a_canonicalPath, bool a_isLandscapeTexture) const;

	private:
		struct TexturePatterns
		{
			bool Matches(std::string_view a_canonicalPath) const;
			void Merge(TexturePatterns& a_other);

			std::unordered_set<std::string> filenames;
			std::unordered_set<std::string> paths;
			std::unordered_set<std::string> directories;
		};

		void LoadFile(const std::filesystem::path& a_path);

		TexturePatterns included;
		TexturePatterns excluded;
	};
}
