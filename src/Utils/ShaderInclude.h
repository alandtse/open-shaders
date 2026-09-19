#pragma once

#include <Windows.h>
#include <cstdint>
#include <d3dcompiler.h>
#include <filesystem>
#include <memory>

namespace Util::ShaderInclude
{
	struct File
	{
		std::unique_ptr<char[]> data;
		UINT size = 0;
	};

	struct ReadError
	{
		const char* operation = "";
		DWORD code = ERROR_SUCCESS;
		uint64_t expectedBytes = 0;
		uint64_t readBytes = 0;
	};

	/** @brief Read include bytes without CRT file descriptors; close the handle before returning. */
	bool Read(const std::filesystem::path& path, File& contents, ReadError& error) noexcept;

	/** @brief Log each distinct include failure once per process, with a bounded diagnostic history. */
	void Report(const std::filesystem::path& source, const std::filesystem::path& attemptedPath, const ReadError& error) noexcept;
}

namespace Util
{
	/** @brief Resolve standalone shader includes under Data/Shaders using Win32 file I/O. */
	struct CustomInclude : public ID3DInclude
	{
		/** @brief Retain the root shader path for failure diagnostics. */
		explicit CustomInclude(const std::filesystem::path& source) : sourcePath(source) {}

		/** @brief Return an owned include buffer, including a non-null buffer for empty files. */
		HRESULT Open(D3D_INCLUDE_TYPE type, LPCSTR filename, LPCVOID parent, LPCVOID* data, UINT* size) noexcept override;
		/** @brief Release the buffer returned by Open. */
		HRESULT Close(LPCVOID data) noexcept override;

	private:
		std::filesystem::path sourcePath;
	};
}
