#pragma once

#include <Windows.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace DX12ResourceService
{
    struct CompanionResult
    {
        DWORD exitCode = ~DWORD{ 0 };
        std::string log;
        std::string error;
    };

    inline std::filesystem::path CompanionExecutable()
    {
        return std::filesystem::absolute(
            "Data/SKSE/Plugins/CommunityShaders/DX12Resources/bin/DX12ResourcesPrep.exe");
    }

    inline std::filesystem::path SkyrimDataDirectory()
    {
        return std::filesystem::absolute("Data");
    }

    inline std::filesystem::path SkyrimPluginsFile()
    {
        PWSTR folder = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &folder)) ||
            !folder) return {};
        const std::filesystem::path root(folder);
        CoTaskMemFree(folder);
        return root / "Skyrim Special Edition" / "Plugins.txt";
    }

    inline std::wstring QuoteWindowsArgument(std::wstring_view text)
    {
        std::wstring result = L"\"";
        size_t backslashes = 0;
        for (const wchar_t ch : text) {
            if (ch == L'\\') { ++backslashes; continue; }
            if (ch == L'"') {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(ch);
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    inline CompanionResult RunCompanion(const std::vector<std::wstring>& arguments,
        const std::filesystem::path& logPath)
    {
        CompanionResult result;
        const auto executable = CompanionExecutable();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(executable, ec)) {
            result.error = "DX12ResourcesPrep.exe is not installed in DX12Resources/bin";
            return result;
        }
        std::filesystem::create_directories(logPath.parent_path(), ec);
        if (ec) { result.error = "Could not create DX12 Resources log directory"; return result; }

        SECURITY_ATTRIBUTES inherit{};
        inherit.nLength = sizeof(inherit);
        inherit.bInheritHandle = TRUE;
        const HANDLE logHandle = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logHandle == INVALID_HANDLE_VALUE) {
            result.error = "Could not create DX12 Resources process log";
            return result;
        }
        std::wstring command = QuoteWindowsArgument(executable.wstring());
        for (const auto& arg : arguments) {
            command.push_back(L' ');
            command += QuoteWindowsArgument(arg);
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = logHandle;
        startup.hStdError = logHandle;
        PROCESS_INFORMATION process{};
        const auto working = std::filesystem::absolute(".").wstring();
        const BOOL launched = CreateProcessW(executable.c_str(), command.data(), nullptr,
            nullptr, TRUE, CREATE_NO_WINDOW, nullptr, working.c_str(), &startup, &process);
        const DWORD launchError = launched ? ERROR_SUCCESS : GetLastError();
        CloseHandle(logHandle);
        if (!launched) {
            result.error = "DX12 Resources companion launch failed (Win32 " +
                std::to_string(launchError) + ")";
            return result;
        }
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
        if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &result.exitCode))
            result.error = "Could not collect DX12 Resources companion exit status";
        CloseHandle(process.hProcess);
        std::ifstream stream(logPath, std::ios::binary | std::ios::ate);
        if (stream) {
            const auto size = stream.tellg();
            if (size > std::streampos{ 0 }) {
                const auto length = std::min<std::streamoff>(std::streamoff(size), 8192);
                stream.seekg(-length, std::ios::end);
                result.log.resize(size_t(length));
                stream.read(result.log.data(), length);
            }
        }
        if (result.error.empty() && result.exitCode != 0)
            result.error = "DX12ResourcesPrep exited with code " + std::to_string(result.exitCode);
        return result;
    }
}
