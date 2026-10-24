#include "FileSystem.h"
#include <Windows.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <psapi.h>

namespace Util
{
	// Path helper utilities implementation
	namespace PathHelpers
	{
		std::filesystem::path GetDataPath()
		{
			try {
				// Get the current process (game) executable path
				wchar_t buffer[MAX_PATH];
				DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
				if (length == 0 || length == MAX_PATH) {
					throw std::runtime_error("Failed to get module filename");
				}

				auto executablePath = std::filesystem::path(buffer);

				auto gamePath = executablePath.parent_path();
				auto dataPath = gamePath / "Data";
				return dataPath;
			} catch (const std::exception& e) {
				// Fallback to current_path if Windows API method fails
				logger::warn("Failed to get game path via Windows API, falling back to current_path: {}", e.what());
				return std::filesystem::current_path() / "Data";
			}
		}

		std::filesystem::path GetCommunityShaderPath()
		{
			return GetDataPath() / "SKSE" / "Plugins" / "CommunityShaders";
		}

		std::filesystem::path GetImGuiIniPath()
		{
			return GetDataPath() / "SKSE" / "Plugins" / "CommunityShaders_ImGui.ini";
		}

		std::filesystem::path GetInterfacePath()
		{
			return GetDataPath() / "Interface" / "CommunityShaders";
		}

		std::filesystem::path GetFontsPath()
		{
			return GetInterfacePath() / "Fonts";
		}

		std::filesystem::path GetIconsPath()
		{
			return GetInterfacePath() / "Icons";
		}

		std::filesystem::path GetIconFontsPath()
		{
			return GetIconsPath() / "glyphs";
		}

		std::filesystem::path GetCursorsPath()
		{
			return GetInterfacePath() / "Cursors";
		}

		std::filesystem::path GetSettingsUserPath()
		{
			return GetCommunityShaderPath() / "SettingsUser.json";
		}

		std::filesystem::path GetSettingsTestPath()
		{
			return GetCommunityShaderPath() / "SettingsTest.json";
		}

		std::filesystem::path GetSettingsDefaultPath()
		{
			return GetCommunityShaderPath() / "SettingsDefault.json";
		}

		std::filesystem::path GetSettingsThemePath()
		{
			return GetCommunityShaderPath() / "SettingsTheme.json";
		}

		std::filesystem::path GetThemesPath()
		{
			return GetCommunityShaderPath() / "Themes";
		}

		std::filesystem::path GetTranslationsPath()
		{
			return GetCommunityShaderPath() / "Translations";
		}

		std::filesystem::path GetCapturesPath()
		{
			return GetCommunityShaderPath() / "Captures";
		}

		std::filesystem::path GetOverridesPath()
		{
			return GetCommunityShaderPath() / "Overrides";
		}

		std::filesystem::path GetUserOverridesPath()
		{
			return GetOverridesPath() / "User";
		}

		std::filesystem::path GetAppliedOverridesPath()
		{
			return GetCommunityShaderPath() / "AppliedOverrides.json";
		}

		std::filesystem::path GetSceneSettingsPath()
		{
			return GetCommunityShaderPath() / "SceneSettings";
		}

		std::filesystem::path GetUnifiedWaterCachePath()
		{
			return GetCommunityShaderPath() / "UnifiedWaterCache";
		}

		std::filesystem::path GetWindSettingsPath()
		{
			return GetCommunityShaderPath() / "WindSettings";
		}

		std::filesystem::path GetShadersPath()
		{
			return GetDataPath() / "Shaders";
		}

		std::filesystem::path GetFeaturesPath()
		{
			return GetShadersPath() / "Features";
		}

		std::filesystem::path GetCurrentModuleRealPath()
		{
			try {
				HMODULE selfModule = nullptr;
				if (!GetModuleHandleExW(
						GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(&GetCurrentModuleRealPath),
						&selfModule)) {
					return {};
				}
				wchar_t buffer[MAX_PATH]{};
				DWORD size = GetModuleFileNameExW(GetCurrentProcess(), selfModule, buffer, MAX_PATH);
				if (size == 0 || size == MAX_PATH) {
					throw std::runtime_error("Failed to get module filename");
				}
				return std::filesystem::path(buffer);
			} catch (const std::exception& e) {
				logger::error("GetCurrentModuleRealPath: Exception caught: {}", e.what());
				return {};
			}
		}

		std::filesystem::path GetRootRealPath()
		{
			static std::filesystem::path cachedPath = []() {
				std::filesystem::path dllPath = GetCurrentModuleRealPath();
				if (dllPath.empty())
					return std::filesystem::path{};
				return dllPath.parent_path().parent_path().parent_path();
			}();
			return cachedPath;
		}

		std::filesystem::path GetShadersRealPath()
		{
			return GetRootRealPath() / "Shaders";
		}

		std::filesystem::path GetThemesRealPath()
		{
			return GetRootRealPath() / "SKSE" / "Plugins" / "CommunityShaders" / "Themes";
		}

		namespace
		{
			/** @brief A CommunityShaders subfolder under the real mod root, or its in-game path when the root is unknown. */
			std::filesystem::path GetCommunityShaderRealPath(const std::filesystem::path& relativePath)
			{
				const auto realRoot = GetRootRealPath();
				return realRoot.empty() ? GetCommunityShaderPath() / relativePath :
				                          realRoot / "SKSE" / "Plugins" / "CommunityShaders" / relativePath;
			}
		}

		std::filesystem::path GetEffects11PresetsRealPath()
		{
			return GetCommunityShaderRealPath(kEffects11PresetsSubdir);
		}

		std::filesystem::path GetUnifiedPresetsRealPath()
		{
			return GetCommunityShaderRealPath(kUnifiedPresetsSubdir);
		}

		std::vector<std::filesystem::path> GetCommunityShaderScanRoots(const std::filesystem::path& relativePath)
		{
			std::vector<std::filesystem::path> roots{ GetCommunityShaderPath() / relativePath };
			if (const auto realRoot = GetRootRealPath(); !realRoot.empty()) {
				auto realPath = realRoot / "SKSE" / "Plugins" / "CommunityShaders" / relativePath;
				std::error_code ec;
				if (!std::filesystem::equivalent(roots.front(), realPath, ec))
					roots.push_back(std::move(realPath));
			}
			return roots;
		}

		std::string ToGenericUtf8(const std::filesystem::path& path)
		{
			const auto text = path.generic_u8string();
			return { reinterpret_cast<const char*>(text.data()), text.size() };
		}

		namespace
		{
			bool IsHiddenLibraryPrefix(wchar_t first)
			{
				return first == L'_' || first == L'.';
			}
		}

		bool IsHiddenLibraryEntry(std::string_view name)
		{
			return !name.empty() && IsHiddenLibraryPrefix(static_cast<unsigned char>(name.front()));
		}

		bool IsHiddenLibraryEntry(const std::filesystem::path& name)
		{
			const auto& native = name.native();
			return !native.empty() && IsHiddenLibraryPrefix(native.front());
		}

		std::vector<std::filesystem::path> ListCommunityShaderEntries(const std::filesystem::path& relativePath, bool directories)
		{
			std::vector<std::filesystem::path> found;
			const auto isListed = [&](const std::filesystem::path& name) {
				return std::ranges::any_of(found, [&](const auto& path) {
					return _wcsicmp(path.filename().c_str(), name.c_str()) == 0;
				});
			};
			for (const auto& root : GetCommunityShaderScanRoots(relativePath)) {
				std::error_code ec;
				for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
					std::error_code typeEc;
					const bool wantedType = directories ? entry.is_directory(typeEc) : entry.is_regular_file(typeEc);
					const auto name = entry.path().filename();
					if (wantedType && !name.empty() && !IsHiddenLibraryEntry(name) && !isListed(name))
						found.push_back(entry.path());
				}
			}
			return found;
		}

		std::vector<std::filesystem::path> GetUnifiedPackPaths(const std::string& packId)
		{
			std::vector<std::filesystem::path> packPaths;
			const auto packFolder = FromUtf8(packId);
			for (const auto& root : GetCommunityShaderScanRoots(kUnifiedPresetsSubdir)) {
				std::error_code ec;
				if (std::filesystem::is_directory(root / packFolder, ec))
					packPaths.push_back(root / packFolder);
			}
			return packPaths;
		}

		std::filesystem::path GetUnifiedPackPath(const std::string& packId)
		{
			const auto packPaths = GetUnifiedPackPaths(packId);
			return packPaths.empty() ? GetCommunityShaderScanRoots(kUnifiedPresetsSubdir).front() / FromUtf8(packId) : packPaths.front();
		}

		std::filesystem::path GetFeaturesRealPath()
		{
			return GetShadersRealPath() / "Features";
		}

		std::filesystem::path GetRealPathFromDataRelative(const std::filesystem::path& dataRelativePath)
		{
			std::filesystem::path root = GetRootRealPath();
			if (root.empty())
				return {};
			auto it = dataRelativePath.begin();
			// Skip leading "Data" — MO2 maps the game's Data directory to the mod's physical root
			if (it != dataRelativePath.end() && _wcsicmp(it->c_str(), L"Data") == 0)
				++it;
			for (; it != dataRelativePath.end(); ++it)
				root /= *it;
			return root;
		}

		std::filesystem::path GetFeatureIniPath(const std::string& featureName)
		{
			return GetFeaturesPath() / (featureName + ".ini");
		}

		std::filesystem::path GetFeatureShaderPath(const std::string& featureName)
		{
			return GetFeaturesPath() / featureName;
		}

		std::filesystem::path GetLogPath()
		{
			auto path = logger::log_directory();
			if (!path) {
				return {};
			}
			*path /= std::format("{}.log", Plugin::LOG_NAME);
			return *path;
		}

		std::filesystem::path SafeAbsolute(const std::filesystem::path& path)
		{
			std::error_code ec;
			auto result = std::filesystem::absolute(path, ec);
			if (ec) {
				logger::warn("Failed to resolve absolute path for '{}': {}", path.string(), ec.message());
				return path;
			}
			return result;
		}

		bool SafeExists(const std::filesystem::path& path)
		{
			std::error_code ec;
			bool result = std::filesystem::exists(path, ec);
			if (ec) {
				logger::warn("Failed to check existence of '{}': {}", path.string(), ec.message());
				return false;
			}
			return result;
		}

		bool SafeIsDirectory(const std::filesystem::path& path)
		{
			std::error_code ec;
			bool result = std::filesystem::is_directory(path, ec);
			if (ec) {
				logger::warn("Failed to check directory status of '{}': {}", path.string(), ec.message());
				return false;
			}
			return result;
		}

		std::filesystem::path SafeRelative(const std::filesystem::path& path, const std::filesystem::path& base)
		{
			std::error_code ec;
			auto result = std::filesystem::relative(path, base, ec);
			if (ec) {
				logger::warn("Failed to resolve '{}' relative to '{}': {}", path.string(), base.string(), ec.message());
				return path;
			}
			return result;
		}

		std::vector<std::filesystem::path> SafeChildDirectories(const std::filesystem::path& root)
		{
			std::vector<std::filesystem::path> children;
			std::error_code ec;
			for (auto it = std::filesystem::directory_iterator(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
				if (it->is_directory(ec))
					children.push_back(SafeAbsolute(it->path()));
			}
			return children;
		}

		bool IsPathLexicallyWithinDirectory(const std::filesystem::path& directory, const std::filesystem::path& path)
		{
			if (directory.empty() || path.empty())
				return false;
			std::error_code ec;
			const auto absoluteDirectory = std::filesystem::absolute(directory, ec);
			if (ec)
				return false;

			const auto absolutePath = std::filesystem::absolute(path, ec);
			if (ec)
				return false;

			const auto relative = absolutePath.lexically_normal().lexically_relative(absoluteDirectory.lexically_normal());
			return !relative.empty() && *relative.begin() != "..";
		}

		std::string ToUtf8(const std::filesystem::path& path)
		{
			if (path.empty())
				return {};
			return stl::utf16_to_utf8(path.native()).value_or("<unprintable path>");
		}

		std::filesystem::path FromUtf8(std::string_view utf8)
		{
			if (utf8.empty())
				return {};
			return stl::utf8_to_utf16(utf8).value_or(std::wstring{});
		}
	}

	// File system utilities implementation
	namespace FileHelpers
	{
		DeletionResult SafeDelete(const std::string& path, const std::string& description)
		{
			DeletionResult result;
			result.deletedDescription = description + ": " + path;

			if (path.empty() || !std::filesystem::exists(path)) {
				result.success = true;  // Consider non-existent files as successfully "deleted"
				return result;
			}

			try {
				if (std::filesystem::is_directory(path)) {
					std::filesystem::remove_all(path);
				} else {
					std::filesystem::remove(path);
				}
				result.success = true;
				logger::info("Deleted {}: {}", description, path);
			} catch (const std::filesystem::filesystem_error& e) {
				result.success = false;
				result.errorMessage = e.what();
				logger::error("Failed to delete {}: {} - {}", description, path, e.what());
			}

			return result;
		}

		void EnsureDirectoryExists(const std::filesystem::path& path)
		{
			std::error_code ec;
			std::filesystem::create_directories(path, ec);
			if (ec) {
				logger::warn("Failed to create directory '{}': {}", Util::PathHelpers::ToUtf8(path), ec.message());
			}
		}

		bool OpenWithShell(const std::filesystem::path& path)
		{
			// ShellExecute returns a value above 32 on success.
			constexpr intptr_t kShellExecuteSuccessThreshold = 32;
			const auto result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			return reinterpret_cast<intptr_t>(result) > kShellExecuteSuccessThreshold;
		}

		std::string SanitizeFileName(std::string name)
		{
			// Trim
			constexpr std::string_view trimLeadingChars = " \t\r\n\v\f-";
			auto first = name.find_first_not_of(trimLeadingChars);
			if (first == std::string::npos)
				return "";
			constexpr std::string_view trimTrailingChars = " \t\r\n\v\f.";
			// Nothing but dots and spaces would leave "." or "..", which name a directory rather than a file.
			const auto last = name.find_last_not_of(trimTrailingChars);
			if (last == std::string::npos || last < first)
				return "";
			name = name.substr(first, last - first + 1);

			// Replace invalid characters
			std::replace_if(name.begin(), name.end(), [](char c) {
				auto u = static_cast<unsigned char>(c);
				// Only perform "illegal" checks if it's a standard ASCII character (0-127)
				if (u < 128u) {
					return c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
					       c == '"' || c == '<' || c == '>' || c == '|' ||
					       u < 32u || u == 127u;
				}
				return false; }, '_');

			// Windows reserved device names
			static constexpr const char* reserved[] = {
				"CON", "PRN", "AUX", "NUL",
				"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
				"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
			};

			for (const char* r : reserved) {
				if (Util::IEquals(name, r)) {
					name += '_';
					break;
				}
			}

			// Limit length, backing off to a UTF-8 lead byte so no character is cut in half.
			constexpr size_t kMaxFileNameBytes = 255;
			if (name.length() > kMaxFileNameBytes) {
				constexpr unsigned char kContinuationMask = 0xC0, kContinuationBits = 0x80;
				size_t length = kMaxFileNameBytes;
				while (length > 0 && (static_cast<unsigned char>(name[length]) & kContinuationMask) == kContinuationBits)
					--length;
				name.resize(length);
			}

			return name;
		}

		namespace
		{
			/** @brief Truncating binary write; true only when the file opened, wrote and closed without error. */
			bool WriteRawContent(const std::filesystem::path& path, std::string_view content)
			{
				std::ofstream file(path, std::ios::binary | std::ios::trunc);
				if (!file.is_open())
					return false;
				file.write(content.data(), static_cast<std::streamsize>(content.size()));
				file.close();
				return !file.fail();
			}
		}

		bool WriteFileAtomically(const std::filesystem::path& path, std::string_view content, std::string_view context)
		{
			std::error_code ec;
			if (!path.parent_path().empty()) {
				std::filesystem::create_directories(path.parent_path(), ec);
				if (ec) {
					logger::error("Could not create directory for {} '{}': {}", context, Util::PathHelpers::ToUtf8(path), ec.message());
					return false;
				}
			}

			// Process and thread qualified so concurrent writers cannot collide on the temporary.
			auto temporaryPath = path;
			temporaryPath += std::format(".{}.{}.tmp", ::GetCurrentProcessId(), ::GetCurrentThreadId());
			if (!WriteRawContent(temporaryPath, content)) {
				logger::error("Could not write temporary {} file '{}'", context, Util::PathHelpers::ToUtf8(temporaryPath));
				std::filesystem::remove(temporaryPath, ec);
				return false;
			}

			if (::MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				return true;
			}

			const auto moveError = ::GetLastError();
			std::filesystem::remove(temporaryPath, ec);
			logger::error("Could not replace {} '{}' (Win32 error {})", context, Util::PathHelpers::ToUtf8(path), moveError);
			return false;
		}

		namespace
		{
			/** @brief Logs why a JSON file was rejected and hands the reason to the caller when requested. */
			void ReportReadFailure(const std::filesystem::path& path, std::string_view context, std::string reason, std::string* error)
			{
				logger::warn("Could not read {} '{}': {}", context, Util::PathHelpers::ToUtf8(path), reason);
				if (error)
					*error = std::move(reason);
			}
		}

		template <class Json>
		std::optional<Json> ReadJsonFile(const std::filesystem::path& path, std::string_view context, std::uintmax_t maxBytes, std::string* error)
		{
			std::ifstream file(path);
			if (!file.is_open())
				return std::nullopt;
			if (maxBytes) {
				std::error_code ec;
				const auto size = std::filesystem::file_size(path, ec);
				if (ec || size > maxBytes) {
					ReportReadFailure(path, context, ec ? ec.message() : std::format("{} bytes exceeds the {} byte limit", size, maxBytes), error);
					return std::nullopt;
				}
			}
			try {
				return Json::parse(file);
			} catch (const std::exception& e) {
				ReportReadFailure(path, context, e.what(), error);
				return std::nullopt;
			}
		}

		template std::optional<nlohmann::json> ReadJsonFile<nlohmann::json>(const std::filesystem::path&, std::string_view, std::uintmax_t, std::string*);
		template std::optional<nlohmann::ordered_json> ReadJsonFile<nlohmann::ordered_json>(const std::filesystem::path&, std::string_view, std::uintmax_t, std::string*);

		std::optional<nlohmann::json> ReadJsonObject(const std::filesystem::path& path, std::string_view context, std::uintmax_t maxBytes, std::string* error)
		{
			auto document = ReadJsonFile(path, context, maxBytes, error);
			if (document && !document->is_object()) {
				ReportReadFailure(path, context, "root is not a JSON object", error);
				return std::nullopt;
			}
			return document;
		}
	}
}

std::vector<SettingsDiffEntry> Util::FileSystem::DiffJson(const nlohmann::json& userJson, const nlohmann::json& testJson, float epsilon)
{
	std::vector<SettingsDiffEntry> diffEntries;

	try {
		auto diff = nlohmann::json::diff(userJson, testJson);

		for (const auto& change : diff) {
			try {
				std::string op = change.value("op", "");
				std::string path = change.value("path", "");
				std::string aVal, bVal;

				if (op == "replace") {
					auto aJson = userJson.at(nlohmann::json::json_pointer(path));
					auto bJson = testJson.at(nlohmann::json::json_pointer(path));

					// If both values are numbers, check if difference is within epsilon (double precision)
					if (aJson.is_number() && bJson.is_number()) {
						double aDouble = aJson.get<double>();
						double bDouble = bJson.get<double>();
						if (std::abs(aDouble - bDouble) < static_cast<double>(epsilon)) {
							continue;  // Skip insignificant numeric differences
						}
					}

					aVal = aJson.dump();
					bVal = bJson.dump();
				} else if (op == "add") {
					aVal = "(none)";
					bVal = testJson.at(nlohmann::json::json_pointer(path)).dump();
				} else if (op == "remove") {
					aVal = userJson.at(nlohmann::json::json_pointer(path)).dump();
					bVal = "(none)";
				} else {
					logger::warn("Unknown JSON diff operation '{}' at path '{}'", op, path);
					continue;
				}

				diffEntries.push_back({ path, aVal, bVal });
			} catch (const std::exception& e) {
				logger::warn("Failed to process JSON diff change: {}", e.what());
				// Continue processing other changes
			}
		}
	} catch (const std::exception& e) {
		logger::warn("Failed to compute JSON diff: {}", e.what());
	}

	return diffEntries;
}
