#include "ShaderInclude.h"

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>
#include <spdlog/spdlog.h>
#include <system_error>
#include <unordered_set>
#include <winrt/base.h>

namespace Util::ShaderInclude
{
	bool Read(const std::filesystem::path& path, File& contents, ReadError& error) noexcept
	{
		contents = {};
		error = {};
		winrt::file_handle handle{ CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (!handle) {
			error = { "open", GetLastError() };
			return false;
		}

		LARGE_INTEGER size;
		if (!GetFileSizeEx(handle.get(), &size)) {
			error = { "size", GetLastError() };
			return false;
		}
		if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > std::numeric_limits<UINT>::max()) {
			error = { "size", ERROR_FILE_TOO_LARGE, static_cast<uint64_t>(size.QuadPart) };
			return false;
		}

		File result;
		result.size = static_cast<UINT>(size.QuadPart);
		try {
			result.data = std::make_unique_for_overwrite<char[]>(std::max<size_t>(result.size, 1));
		} catch (const std::bad_alloc&) {
			error = { "allocate", ERROR_NOT_ENOUGH_MEMORY, result.size };
			return false;
		}

		UINT totalRead = 0;
		while (totalRead < result.size) {
			DWORD bytesRead = 0;
			if (!ReadFile(handle.get(), result.data.get() + totalRead, result.size - totalRead, &bytesRead, nullptr)) {
				error = { "read", GetLastError(), result.size, totalRead };
				return false;
			}
			if (bytesRead == 0) {
				error = { "read", ERROR_HANDLE_EOF, result.size, totalRead };
				return false;
			}
			totalRead += bytesRead;
		}
		contents = std::move(result);
		return true;
	}

	void Report(const std::filesystem::path& source, const std::filesystem::path& attemptedPath, const ReadError& error) noexcept
	{
		try {
			constexpr size_t kMaxReports = 128;
			static std::mutex mutex;
			static std::unordered_set<std::string> reported;
			const auto key = std::format("{}|{}|{}|{}", source.string(), attemptedPath.string(), error.operation, error.code);
			{
				std::lock_guard lock(mutex);
				if (reported.size() >= kMaxReports || !reported.insert(key).second)
					return;
			}
			spdlog::error("[ShaderInclude] source='{}' path='{}' operation={} win32_error={} ({}) expected_bytes={} read_bytes={}",
				source.string(), attemptedPath.string(), error.operation, error.code,
				std::system_category().message(static_cast<int>(error.code)), error.expectedBytes, error.readBytes);
		} catch (...) {
			OutputDebugStringA("[ShaderInclude] Unable to format include failure diagnostics.\n");
		}
	}
}

namespace Util
{
	HRESULT CustomInclude::Open([[maybe_unused]] D3D_INCLUDE_TYPE type, LPCSTR filename,
		[[maybe_unused]] LPCVOID parent, LPCVOID* data, UINT* size) noexcept
	{
		*data = nullptr;
		*size = 0;
		std::filesystem::path path;
		try {
			path = std::filesystem::path(L"Data\\Shaders") / filename;
			ShaderInclude::File contents;
			ShaderInclude::ReadError error;
			if (!ShaderInclude::Read(path, contents, error)) {
				ShaderInclude::Report(sourcePath, path, error);
				return HRESULT_FROM_WIN32(error.code);
			}
			*size = contents.size;
			*data = contents.data.release();
			return S_OK;
		} catch (const std::bad_alloc&) {
			ShaderInclude::Report(sourcePath, path, { "prepare_include", ERROR_NOT_ENOUGH_MEMORY });
			return E_OUTOFMEMORY;
		} catch (...) {
			ShaderInclude::Report(sourcePath, path, { "prepare_include", ERROR_UNHANDLED_EXCEPTION });
			return E_FAIL;
		}
	}

	HRESULT CustomInclude::Close(LPCVOID data) noexcept
	{
		delete[] static_cast<const char*>(data);
		return S_OK;
	}
}
