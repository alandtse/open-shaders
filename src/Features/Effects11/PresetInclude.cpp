#include "PresetInclude.h"

#include "Utils/ShaderInclude.h"

#include <algorithm>
#include <string_view>

namespace EffectSourceCompatibility
{
	PresetInclude::PresetInclude(const std::filesystem::path& a_basePath) :
		basePath(std::filesystem::weakly_canonical(a_basePath)) {}

	HRESULT PresetInclude::ReadInclude(const std::filesystem::path& a_path, LPCVOID* a_data, UINT* a_bytes)
	{
		const auto path = std::filesystem::weakly_canonical(a_path);
		const auto relative = path.lexically_relative(basePath);
		if (relative.empty() || relative.is_absolute() || *relative.begin() == L"..")
			return E_ACCESSDENIED;

		Util::ShaderInclude::File contents;
		Util::ShaderInclude::ReadError error;
		if (!Util::ShaderInclude::Read(path, contents, error))
			return HRESULT_FROM_WIN32(error.code);

		const auto directory = path.parent_path();
		if (std::ranges::find(includeDirectories, directory) == includeDirectories.end())
			includeDirectories.push_back(directory);
		const auto data = contents.data.get();
		openIncludes.emplace(data, IncludeData{ std::move(contents.data), directory });
		*a_data = data;
		*a_bytes = contents.size;
		return S_OK;
	}

	HRESULT __stdcall PresetInclude::Open(D3D_INCLUDE_TYPE a_type, LPCSTR a_fileName, LPCVOID a_parentData, LPCVOID* a_data, UINT* a_bytes)
	{
		*a_data = nullptr;
		*a_bytes = 0;
		try {
			std::string_view name(a_fileName);
			const bool rooted = name.starts_with('/') || name.starts_with('\\');
			while (name.starts_with('/') || name.starts_with('\\'))
				name.remove_prefix(1);
			const std::filesystem::path includeName(name);
			if (includeName.empty() || includeName.has_root_path())
				return E_INVALIDARG;

			std::vector<std::filesystem::path> directories;
			if (!rooted && a_type == D3D_INCLUDE_LOCAL) {
				if (const auto parent = openIncludes.find(a_parentData); parent != openIncludes.end())
					directories.push_back(parent->second.directory);
			}
			directories.push_back(basePath);
			if (!rooted)
				directories.insert(directories.end(), includeDirectories.begin(), includeDirectories.end());

			for (const auto& directory : directories) {
				const auto result = ReadInclude(directory / includeName, a_data, a_bytes);
				if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && result != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
					return result;
			}

			if (std::ranges::find(missingIncludes, a_fileName) == missingIncludes.end())
				missingIncludes.emplace_back(a_fileName);
			auto source = std::make_unique<char[]>(1);
			source[0] = '\n';
			const auto data = source.get();
			openIncludes.emplace(data, IncludeData{ std::move(source), basePath });
			*a_data = data;
			*a_bytes = 1;
			return S_OK;
		} catch (const std::bad_alloc&) {
			return E_OUTOFMEMORY;
		} catch (const std::filesystem::filesystem_error&) {
			return E_FAIL;
		}
	}

	HRESULT __stdcall PresetInclude::Close(LPCVOID a_data)
	{
		return openIncludes.erase(a_data) ? S_OK : E_INVALIDARG;
	}
}
