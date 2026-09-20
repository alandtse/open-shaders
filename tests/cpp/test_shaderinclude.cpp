#include "Utils/ShaderInclude.h"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <vector>
#include <winioctl.h>
#include <winrt/base.h>

namespace
{
	struct IncludeFixture
	{
		std::filesystem::path directory;
		std::filesystem::path path;
		std::filesystem::path previousDirectory = std::filesystem::current_path();
		std::ostringstream output;
		std::shared_ptr<spdlog::logger> previousLogger = spdlog::default_logger();

		IncludeFixture()
		{
			wchar_t temp[MAX_PATH];
			wchar_t file[MAX_PATH];
			REQUIRE(GetTempPathW(MAX_PATH, temp) != 0);
			REQUIRE(GetTempFileNameW(temp, L"sio", 0, file) != 0);
			directory = file;
			std::filesystem::remove(directory);
			std::filesystem::create_directories(directory / "Data/Shaders/Common");
			path = directory / "Data/Shaders/Common/Test.hlsli";
			std::ofstream(path, std::ios::binary) << "include content";
			std::filesystem::current_path(directory);
			auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
			auto log = std::make_shared<spdlog::logger>("include-test", sink);
			log->set_pattern("%v");
			spdlog::set_default_logger(log);
		}

		~IncludeFixture()
		{
			spdlog::set_default_logger(previousLogger);
			std::error_code error;
			std::filesystem::current_path(previousDirectory, error);
			std::filesystem::remove_all(directory, error);
		}
	};

	struct CrtExhaustion
	{
		std::vector<FILE*> streams;
		std::vector<int> descriptors;
		int error = 0;

		explicit CrtExhaustion(bool lowLevel)
		{
			if (lowLevel) {
				for (;;) {
					const int descriptor = _open("NUL", _O_RDONLY | _O_BINARY);
					if (descriptor == -1) {
						error = errno;
						break;
					}
					descriptors.push_back(descriptor);
				}
			} else {
				for (;;) {
					FILE* stream = nullptr;
					const auto result = fopen_s(&stream, "NUL", "rb");
					if (result != 0) {
						error = result;
						break;
					}
					streams.push_back(stream);
				}
			}
		}

		~CrtExhaustion()
		{
			for (auto* stream : streams)
				fclose(stream);
			for (int descriptor : descriptors)
				_close(descriptor);
		}
	};
}

TEST_CASE_METHOD(IncludeFixture, "Win32 include reads succeed with exhausted CRT streams and descriptors", "[shader-include]")
{
	bool lowLevel = false;
	SECTION("stdio stream table") { lowLevel = false; }
	SECTION("low-level descriptor table") { lowLevel = true; }

	Util::ShaderInclude::File file;
	Util::ShaderInclude::ReadError error;
	bool readOK;
	bool crtOpened;
	int crtError;
	int exhaustionError;
	HRESULT includeResult;
	LPCVOID data = nullptr;
	UINT size = 0;
	Util::CustomInclude handler("Root.hlsl");
	{
		CrtExhaustion exhausted(lowLevel);
		exhaustionError = exhausted.error;
		std::ifstream stream(path, std::ios::binary);
		crtError = errno;
		crtOpened = stream.is_open();
		readOK = Util::ShaderInclude::Read(path, file, error);
		includeResult = handler.Open(D3D_INCLUDE_LOCAL, "Common/Test.hlsli", nullptr, &data, &size);
	}
	std::unique_ptr<const char[]> included(static_cast<const char*>(data));
	REQUIRE(exhaustionError == EMFILE);
	CHECK_FALSE(crtOpened);
	CHECK(crtError == EMFILE);
	REQUIRE(readOK);
	CHECK(std::string(file.data.get(), file.size) == "include content");
	REQUIRE(SUCCEEDED(includeResult));
	CHECK(std::string(included.get(), size) == "include content");
	CHECK(output.str().empty());
}

TEST_CASE_METHOD(IncludeFixture, "Nested shader compilation matches bytecode under CRT exhaustion", "[shader-include]")
{
	std::ofstream("Data/Shaders/Root.hlsl") << "#include \"Common/Parent.hlsli\"\nfloat4 main() : SV_Target { return VALUE; }";
	std::ofstream("Data/Shaders/Common/Parent.hlsli") << "#include \"Common/Empty.hlsli\"\n#include \"Common/Child.hlsli\"";
	std::ofstream("Data/Shaders/Common/Empty.hlsli");
	std::ofstream("Data/Shaders/Common/Child.hlsli") << "#define VALUE float4(1, 2, 3, 4)\n";
	winrt::com_ptr<ID3DBlob> baseline;
	winrt::com_ptr<ID3DBlob> baselineErrors;
	Util::CustomInclude baselineHandler("Data/Shaders/Root.hlsl");
	REQUIRE(SUCCEEDED(D3DCompileFromFile(L"Data/Shaders/Root.hlsl", nullptr, &baselineHandler, "main", "ps_5_0",
		0, 0, baseline.put(), baselineErrors.put())));

	bool lowLevel = false;
	SECTION("stdio stream table") { lowLevel = false; }
	SECTION("low-level descriptor table") { lowLevel = true; }
	winrt::com_ptr<ID3DBlob> compiled;
	winrt::com_ptr<ID3DBlob> compileErrors;
	Util::CustomInclude handler("Data/Shaders/Root.hlsl");
	HRESULT result;
	int exhaustionError;
	{
		CrtExhaustion exhausted(lowLevel);
		exhaustionError = exhausted.error;
		result = D3DCompileFromFile(L"Data/Shaders/Root.hlsl", nullptr, &handler, "main", "ps_5_0",
			0, 0, compiled.put(), compileErrors.put());
	}
	INFO((compileErrors ? static_cast<const char*>(compileErrors->GetBufferPointer()) : "No compiler errors"));
	REQUIRE(exhaustionError == EMFILE);
	REQUIRE(SUCCEEDED(result));
	REQUIRE(compiled->GetBufferSize() == baseline->GetBufferSize());
	CHECK(std::memcmp(compiled->GetBufferPointer(), baseline->GetBufferPointer(), compiled->GetBufferSize()) == 0);
}

TEST_CASE_METHOD(IncludeFixture, "Missing includes preserve Windows errors and suppress duplicate reports", "[shader-include]")
{
	std::filesystem::remove(path);
	Util::CustomInclude handler("Root.hlsl");
	const char sentinel = 0;
	LPCVOID data = &sentinel;
	UINT size = 10;
	CHECK(handler.Open(D3D_INCLUDE_LOCAL, "Common/Test.hlsli", nullptr, &data, &size) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
	CHECK(data == nullptr);
	CHECK(size == 0);
	const auto first = output.str();
	CHECK(first.find("operation=open") != std::string::npos);
	CHECK(first.find("win32_error=2 (") != std::string::npos);
	CHECK(first.find("source='Root.hlsl'") != std::string::npos);
	CHECK(FAILED(handler.Open(D3D_INCLUDE_LOCAL, "Common/Test.hlsli", nullptr, &data, &size)));
	CHECK(output.str() == first);
}

TEST_CASE_METHOD(IncludeFixture, "Win32 include reader preserves sharing violations", "[shader-include]")
{
	winrt::file_handle blocker{ CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	REQUIRE(blocker);
	Util::ShaderInclude::File file;
	Util::ShaderInclude::ReadError error;
	CHECK_FALSE(Util::ShaderInclude::Read(path, file, error));
	CHECK(error.code == ERROR_SHARING_VIOLATION);
	CHECK_FALSE(file.data);
	CHECK(file.size == 0);
}

TEST_CASE_METHOD(IncludeFixture, "Include bytes survive handle closure and subsequent file edits", "[shader-include]")
{
	Util::ShaderInclude::File first;
	Util::ShaderInclude::ReadError error;
	REQUIRE(Util::ShaderInclude::Read(path, first, error));
	winrt::file_handle writer{ CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	REQUIRE(writer);
	const char edited[] = "edited\r\n\0bytes";
	DWORD written;
	REQUIRE(WriteFile(writer.get(), edited, sizeof(edited) - 1, &written, nullptr));
	writer.close();
	Util::ShaderInclude::File second;
	REQUIRE(Util::ShaderInclude::Read(path, second, error));
	CHECK(std::string(first.data.get(), first.size) == "include content");
	CHECK(std::string(second.data.get(), second.size) == std::string(edited, sizeof(edited) - 1));
}

TEST_CASE_METHOD(IncludeFixture, "Empty includes provide a valid zero-length buffer", "[shader-include]")
{
	std::ofstream(path, std::ios::binary | std::ios::trunc);
	Util::ShaderInclude::File file;
	Util::ShaderInclude::ReadError error;
	REQUIRE(Util::ShaderInclude::Read(path, file, error));
	CHECK(file.data != nullptr);
	CHECK(file.size == 0);
}

TEST_CASE_METHOD(IncludeFixture, "Oversized includes fail before buffer allocation", "[shader-include]")
{
	winrt::file_handle writer{ CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	REQUIRE(writer);
	DWORD returned = 0;
	REQUIRE(DeviceIoControl(writer.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
	LARGE_INTEGER size;
	size.QuadPart = static_cast<int64_t>(UINT_MAX) + 1;
	REQUIRE(SetFilePointerEx(writer.get(), size, nullptr, FILE_BEGIN));
	REQUIRE(SetEndOfFile(writer.get()));
	writer.close();
	Util::ShaderInclude::File file;
	Util::ShaderInclude::ReadError error;
	CHECK_FALSE(Util::ShaderInclude::Read(path, file, error));
	CHECK(error.code == ERROR_FILE_TOO_LARGE);
	CHECK(error.expectedBytes == static_cast<uint64_t>(size.QuadPart));
	CHECK_FALSE(file.data);
	CHECK(file.size == 0);
}
