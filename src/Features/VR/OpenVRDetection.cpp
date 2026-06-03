#include "OpenVRDetection.h"
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <format>
#include <openvr.h>
#include <string_view>
#include <vector>
#include <windows.h>
#include <winver.h>
#pragma comment(lib, "version.lib")

namespace
{
	struct OpenCompositeSettingValue
	{
		bool value = false;
		std::string configPath;
	};

	struct OpenCompositeUpscalingSettings
	{
		OpenCompositeSettingValue dlssEnabled;
		OpenCompositeSettingValue fsrEnabled;
		OpenCompositeSettingValue dlaaEnabled;
		OpenCompositeSettingValue fsrNativeAA;
		OpenCompositeSettingValue fsr3PostAAEnabled;
	};

	std::string_view TrimAsciiWhitespace(std::string_view value)
	{
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
			value.remove_prefix(1);
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
			value.remove_suffix(1);
		return value;
	}

	std::string ToLowerAscii(std::string_view value)
	{
		std::string result(value);
		std::ranges::transform(result, result.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return result;
	}

	bool TryParseOpenCompositeBool(std::string value, bool& outValue)
	{
		value = ToLowerAscii(TrimAsciiWhitespace(value));
		if (value == "true" || value == "on" || value == "enabled" || value == "1") {
			outValue = true;
			return true;
		}
		if (value == "false" || value == "off" || value == "disabled" || value == "0") {
			outValue = false;
			return true;
		}
		return false;
	}

	void AddUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path)
	{
		if (path.empty())
			return;

		auto normalized = path.lexically_normal().wstring();
		std::ranges::transform(normalized, normalized.begin(), [](wchar_t c) {
			return static_cast<wchar_t>(std::towlower(c));
		});

		const bool alreadyAdded = std::ranges::any_of(paths, [&](const std::filesystem::path& existing) {
			auto existingNormalized = existing.lexically_normal().wstring();
			std::ranges::transform(existingNormalized, existingNormalized.begin(), [](wchar_t c) {
				return static_cast<wchar_t>(std::towlower(c));
			});
			return existingNormalized == normalized;
		});
		if (!alreadyAdded)
			paths.push_back(path);
	}

	std::filesystem::path GetCurrentDirectoryPath()
	{
		std::wstring buffer(MAX_PATH, L'\0');
		const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
		if (length == 0)
			return {};

		if (length >= buffer.size()) {
			buffer.resize(length + 1);
			const DWORD retryLength = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
			if (retryLength == 0 || retryLength >= buffer.size())
				return {};
			buffer.resize(retryLength);
		} else {
			buffer.resize(length);
		}

		return std::filesystem::path(buffer);
	}

	// Full path of the already-loaded openvr_api.dll, or empty if it isn't
	// loaded. Shared by the config probe and GatherDLLInfo so the module lookup
	// lives in one place.
	std::filesystem::path GetLoadedOpenVRDllPath()
	{
		HMODULE openVRModule = GetModuleHandleW(L"openvr_api.dll");
		if (!openVRModule)
			return {};

		std::wstring buffer(MAX_PATH, L'\0');
		const DWORD length = GetModuleFileNameW(openVRModule, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size())
			return {};

		buffer.resize(length);
		return std::filesystem::path(buffer);
	}

	std::filesystem::path GetLoadedOpenVRDirectory()
	{
		return GetLoadedOpenVRDllPath().parent_path();
	}

	std::vector<std::filesystem::path> GetOpenCompositeConfigCandidates()
	{
		std::vector<std::filesystem::path> candidates;

		const auto loadedOpenVRDirectory = GetLoadedOpenVRDirectory();
		if (!loadedOpenVRDirectory.empty()) {
			AddUniquePath(candidates, loadedOpenVRDirectory / L"opencomposite.ini");
			AddUniquePath(candidates, loadedOpenVRDirectory / L"opencomposite_ext.ini");
		}

		const auto currentDirectory = GetCurrentDirectoryPath();
		if (!currentDirectory.empty()) {
			AddUniquePath(candidates, currentDirectory / L"opencomposite.ini");
			AddUniquePath(candidates, currentDirectory / L"opencomposite_ext.ini");
		}

		return candidates;
	}

	// OpenComposite writes upscaling keys at top level or under a section
	// depending on build, so probe the unnamed section first then every named
	// one.
	bool TryReadIniBoolSetting(const CSimpleIniA& ini, const char* key, bool& outValue)
	{
		auto tryReadSection = [&](const char* section) {
			const char* rawValue = ini.GetValue(section, key, nullptr);
			return rawValue && TryParseOpenCompositeBool(rawValue, outValue);
		};

		if (tryReadSection(""))
			return true;

		CSimpleIniA::TNamesDepend sections;
		ini.GetAllSections(sections);
		for (const auto& section : sections) {
			if (section.pItem && tryReadSection(section.pItem))
				return true;
		}

		return false;
	}

	void UpdateOpenCompositeSettingValue(OpenCompositeSettingValue& setting, const CSimpleIniA& ini, const char* key, const std::filesystem::path& path)
	{
		// Enabled-by-any-config wins: only an explicit true updates the setting, so
		// a later file's false can't clobber an earlier file's true. configPath
		// then names the file that enabled it (for the UI/log message).
		bool parsedValue = false;
		if (!TryReadIniBoolSetting(ini, key, parsedValue) || !parsedValue)
			return;

		setting.value = true;
		setting.configPath = path.string();
	}

	OpenCompositeUpscalingSettings ReadOpenCompositeUpscalingSettings()
	{
		OpenCompositeUpscalingSettings settings;

		std::error_code ec;
		for (const auto& path : GetOpenCompositeConfigCandidates()) {
			if (!std::filesystem::exists(path, ec))
				continue;
			ec.clear();

			CSimpleIniA ini;
			ini.SetUnicode();
			const SI_Error rc = ini.LoadFile(path.c_str());
			if (rc < 0) {
				logger::warn("[VRDetection] Failed to read OpenComposite config '{}': {}", path.string(), static_cast<int>(rc));
				continue;
			}

			UpdateOpenCompositeSettingValue(settings.dlssEnabled, ini, "dlssEnabled", path);
			UpdateOpenCompositeSettingValue(settings.fsrEnabled, ini, "fsrEnabled", path);
			UpdateOpenCompositeSettingValue(settings.dlaaEnabled, ini, "dlaaEnabled", path);
			UpdateOpenCompositeSettingValue(settings.fsrNativeAA, ini, "fsrNativeAA", path);
			UpdateOpenCompositeSettingValue(settings.fsr3PostAAEnabled, ini, "fsr3PostAAEnabled", path);
		}

		return settings;
	}
}

namespace VRDetection
{
	const char* RuntimeTypeToString(RuntimeType type)
	{
		switch (type) {
		case RuntimeType::SteamVR:
			return "SteamVR";
		case RuntimeType::OpenComposite:
			return "OpenComposite";
		default:
			return "Unknown";
		}
	}

	bool ProbeRuntimeInterfaces(OpenVRDetectionResult& result)
	{
		HMODULE hModule = GetModuleHandleA("openvr_api.dll");
		if (!hModule)
			return false;

		using pfnIsValid = bool(__cdecl*)(const char*);
		auto IsValid = reinterpret_cast<pfnIsValid>(GetProcAddress(hModule, "VR_IsInterfaceVersionValid"));
		if (!IsValid)
			return false;

		result.hasOverlayInterface = IsValid(vr::IVROverlay_Version);
		result.hasSystemInterface = IsValid(vr::IVRSystem_Version);
		result.hasCompositorInterface = IsValid(vr::IVRCompositor_Version);

		result.probingSucceeded = result.hasOverlayInterface && result.hasSystemInterface && result.hasCompositorInterface;
		return result.probingSucceeded;
	}

	void GatherDLLInfo(OpenVRDetectionResult& result)
	{
		const auto dllPathFs = GetLoadedOpenVRDllPath();
		if (dllPathFs.empty()) {
			result.isAvailable = false;
			return;
		}

		result.isAvailable = true;

		const std::string dllPath = dllPathFs.string();
		result.dllPath = dllPath;

		DWORD dwSize = GetFileVersionInfoSizeA(dllPath.c_str(), nullptr);
		if (dwSize > 0) {
			std::vector<BYTE> buffer(dwSize);
			if (GetFileVersionInfoA(dllPath.c_str(), 0, dwSize, buffer.data())) {
				VS_FIXEDFILEINFO* pFileInfo = nullptr;
				UINT len = 0;
				if (VerQueryValueA(buffer.data(), "\\", reinterpret_cast<LPVOID*>(&pFileInfo), &len)) {
					DWORD major = HIWORD(pFileInfo->dwFileVersionMS);
					DWORD minor = LOWORD(pFileInfo->dwFileVersionMS);
					DWORD build = HIWORD(pFileInfo->dwFileVersionLS);
					DWORD revision = LOWORD(pFileInfo->dwFileVersionLS);
					result.version = std::format("{}.{}.{}.{}", major, minor, build, revision);
				}
			}
		}

		if (result.version.empty())
			result.version = "Unknown";

		WIN32_FIND_DATAA findData;
		HANDLE hFind = FindFirstFileA(dllPath.c_str(), &findData);
		if (hFind != INVALID_HANDLE_VALUE) {
			FindClose(hFind);
			ULARGE_INTEGER fileSize;
			fileSize.LowPart = findData.nFileSizeLow;
			fileSize.HighPart = findData.nFileSizeHigh;
			result.fileSize = fileSize.QuadPart;

			SYSTEMTIME st;
			FileTimeToSystemTime(&findData.ftLastWriteTime, &st);
			result.modificationTime = std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		}
	}

	RuntimeType DetectRuntimeType(const std::string& dllPath, const std::string& version, uint64_t fileSize)
	{
		// OpenComposite DLLs are typically small (~600KB) with version 1.0.10.0
		if (version == "1.0.10.0" && fileSize < 700000)
			return RuntimeType::OpenComposite;

		// Check path for OpenComposite indicators
		std::string lowerPath = dllPath;
		for (auto& c : lowerPath)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

		if (lowerPath.find("opencomposite") != std::string::npos)
			return RuntimeType::OpenComposite;

		// SteamVR DLLs are typically larger and have higher version numbers
		if (lowerPath.find("steamvr") != std::string::npos || lowerPath.find("steam") != std::string::npos)
			return RuntimeType::SteamVR;

		// Higher version numbers suggest SteamVR
		if (!version.empty() && version != "Unknown" && version != "1.0.10.0")
			return RuntimeType::SteamVR;

		return RuntimeType::Unknown;
	}

	OpenVRDetectionResult Detect()
	{
		OpenVRDetectionResult result;

		GatherDLLInfo(result);
		if (!result.isAvailable)
			return result;

		result.runtimeType = DetectRuntimeType(result.dllPath, result.version, result.fileSize);

		// Detect compatibility via runtime interface probing
		result.isCompatible = ProbeRuntimeInterfaces(result);

		return result;
	}

	OpenCompositeUpscalingState DetectOpenCompositeUpscaling()
	{
		const auto settings = ReadOpenCompositeUpscalingSettings();

		OpenCompositeUpscalingState state;
		auto setActive = [&](const char* settingName, const OpenCompositeSettingValue& setting) {
			state.active = true;
			state.settingName = settingName;
			state.configPath = setting.configPath;
		};

		// Report the most representative enabled setting (AA modes first) as the
		// trigger shown in the UI/log.
		if (settings.dlaaEnabled.value)
			setActive("dlaaEnabled", settings.dlaaEnabled);
		else if (settings.fsrNativeAA.value)
			setActive("fsrNativeAA", settings.fsrNativeAA);
		else if (settings.fsr3PostAAEnabled.value)
			setActive("fsr3PostAAEnabled", settings.fsr3PostAAEnabled);
		else if (settings.dlssEnabled.value)
			setActive("dlssEnabled", settings.dlssEnabled);
		else if (settings.fsrEnabled.value)
			setActive("fsrEnabled", settings.fsrEnabled);

		return state;
	}
}
