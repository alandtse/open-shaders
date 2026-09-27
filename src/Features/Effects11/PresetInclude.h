#pragma once

#include <d3dcompiler.h>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace EffectSourceCompatibility
{
	/// @brief Resolves preset includes and preserves compatibility with absent legacy helpers.
	class PresetInclude : public ID3DInclude
	{
	public:
		/// @brief Uses the effect's directory as the preset include root.
		explicit PresetInclude(const std::filesystem::path& a_basePath);
		/// @brief Loads an include relative to its parent, the preset root, or a previously loaded directory.
		HRESULT __stdcall Open(D3D_INCLUDE_TYPE a_type, LPCSTR a_fileName, LPCVOID a_parentData, LPCVOID* a_data, UINT* a_bytes) override;
		/// @brief Releases the compiler's include buffer.
		HRESULT __stdcall Close(LPCVOID a_data) override;
		/// @brief Lists missing includes accepted as empty source during this compilation.
		const std::vector<std::string>& GetMissingIncludes() const { return missingIncludes; }

	private:
		struct IncludeData
		{
			std::unique_ptr<char[]> source;
			std::filesystem::path directory;
		};

		HRESULT ReadInclude(const std::filesystem::path& a_path, LPCVOID* a_data, UINT* a_bytes);

		std::filesystem::path basePath;
		std::vector<std::filesystem::path> includeDirectories;
		std::unordered_map<LPCVOID, IncludeData> openIncludes;
		std::vector<std::string> missingIncludes;
	};
}
