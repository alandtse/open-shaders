#include "PCH.h"
#include "NativeDXC.h"
#include "Globals.h"
#include "State.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

#if __has_include(<dxcapi.h>)
#include <dxcapi.h>
#define CS_RUNTIME_DXC_AVAILABLE 1
#else
#define CS_RUNTIME_DXC_AVAILABLE 0
#endif

namespace
{
    constexpr std::streamoff kMaximumShaderBytes = 16 * 1024 * 1024;

    std::vector<char> ReadBytecode(const char* path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
            throw std::runtime_error(std::string("Native shader unavailable: ") + path);
        const auto size = static_cast<std::streamoff>(input.tellg());
        if (size <= 0 || size > kMaximumShaderBytes)
            throw std::runtime_error(std::string("Native shader size invalid: ") + path);
        std::vector<char> result(static_cast<size_t>(size));
        input.seekg(0);
        if (!input.read(result.data(), static_cast<std::streamsize>(result.size())))
            throw std::runtime_error(std::string("Native shader read failed: ") + path);
        return result;
    }

#if CS_RUNTIME_DXC_AVAILABLE
    struct CompilerModule
    {
        HMODULE handle = LoadLibraryExW(L"dxcompiler.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        ~CompilerModule() { if (handle) FreeLibrary(handle); }
        CompilerModule(const CompilerModule&) = delete;
        CompilerModule& operator=(const CompilerModule&) = delete;
        CompilerModule() = default;
    };

    std::vector<char> Compile(const char* sourcePath, bool reverseZ)
    {
        const auto source = ReadBytecode(sourcePath);
        CompilerModule library;
        if (!library.handle)
            throw std::runtime_error("dxcompiler.dll is unavailable in the application DLL search path");
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(library.handle, "DxcCreateInstance"));
        if (!create)
            throw std::runtime_error("dxcompiler.dll does not export DxcCreateInstance");

        winrt::com_ptr<IDxcUtils> utils;
        winrt::com_ptr<IDxcCompiler3> compiler;
        if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(utils.put()))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put()))))
            throw std::runtime_error("Could not initialize runtime DXC");

        winrt::com_ptr<IDxcIncludeHandler> includes;
        if (FAILED(utils->CreateDefaultIncludeHandler(includes.put())))
            throw std::runtime_error("Could not create DXC include handler");

        const std::wstring shaderDir = std::filesystem::path(sourcePath).parent_path().wstring();
        const std::wstring shaderRoot = L"Data/Shaders";
        std::vector<LPCWSTR> arguments{
            L"-E", L"main", L"-T", L"cs_6_5", L"-HV", L"2021", L"-O3",
            L"-Ges", L"-Gis", L"-I", shaderDir.c_str(), L"-I", shaderRoot.c_str()
        };
        if (reverseZ) {
            arguments.push_back(L"-D");
            arguments.push_back(L"REVERSE_Z=1");
        }
        const DxcBuffer buffer{ source.data(), source.size(), DXC_CP_UTF8 };
        winrt::com_ptr<IDxcResult> result;
        const HRESULT compileCall = compiler->Compile(&buffer, arguments.data(),
            static_cast<UINT32>(arguments.size()), includes.get(), IID_PPV_ARGS(result.put()));
        if (FAILED(compileCall) || !result)
            throw std::runtime_error("DXC invocation failed");

        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        if (FAILED(status)) {
            winrt::com_ptr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.put()), nullptr);
            const std::string detail = errors && errors->GetStringLength()
                ? std::string(errors->GetStringPointer(), errors->GetStringLength())
                : "unknown DXC compiler error";
            throw std::runtime_error(std::string("DXC: ") + detail);
        }

        winrt::com_ptr<IDxcBlob> object;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.put()), nullptr)) ||
            !object || !object->GetBufferSize() || object->GetBufferSize() > static_cast<size_t>(kMaximumShaderBytes))
            throw std::runtime_error("DXC produced no valid shader object");
        const auto* begin = static_cast<const char*>(object->GetBufferPointer());
        return std::vector<char>(begin, begin + object->GetBufferSize());
    }
#else
    std::vector<char> Compile(const char*, bool)
    {
        throw std::runtime_error("Runtime DXC was not built: dxcapi.h is unavailable");
    }
#endif
}

std::vector<char> NativeDXC::Load(const char* packagedPath, const char* sourcePath, bool reverseZ)
{
    const int mode = globals::state ? globals::state->nativeDXCMode.load(std::memory_order_relaxed) : 0;
    if (mode != 0) {
        try {
            auto shader = Compile(sourcePath, reverseZ);
            logger::info("Runtime DXC compiled {} (SM 6.5{})", sourcePath, reverseZ ? ", reverse-Z" : "");
            return shader;
        } catch (const std::exception& error) {
            logger::error("Runtime DXC failed for {}: {}", sourcePath, error.what());
            if (mode == 2)
                throw;
            logger::warn("Falling back to packaged DXIL: {}", packagedPath);
        }
    }
    return ReadBytecode(packagedPath);
}
