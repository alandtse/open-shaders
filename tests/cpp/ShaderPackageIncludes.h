#pragma once

#include <cstdint>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>

/// Resolve shader includes from the installed Shaders root, as Util::CompileShader does.
class PackageIncludes : public ID3DInclude
{
public:
	explicit PackageIncludes(std::filesystem::path root, std::filesystem::path featureRoot = {}) :
		root_(std::move(root)), featureRoot_(std::move(featureRoot)) {}
	HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID,
		LPCVOID* data, UINT* bytes) override
	{
		try {
			std::ifstream file(root_ / name, std::ios::binary | std::ios::ate);
			if (!file && !featureRoot_.empty()) {
				file.clear();
				file.open(featureRoot_ / name, std::ios::binary | std::ios::ate);
			}
			if (!file)
				return E_FAIL;
			const auto length = file.tellg();
			if (length < 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<UINT>::max())
				return E_FAIL;
			auto contents = std::make_unique<char[]>(static_cast<std::size_t>(length) + 1u);
			file.seekg(0);
			file.read(contents.get(), length);
			if (!file)
				return E_FAIL;
			*bytes = static_cast<UINT>(length);
			*data = contents.release();
			return S_OK;
		} catch (...) {
			return E_FAIL;
		}
	}
	HRESULT __stdcall Close(LPCVOID data) override
	{
		delete[] static_cast<const char*>(data);
		return S_OK;
	}

private:
	std::filesystem::path root_;
	std::filesystem::path featureRoot_;
};
