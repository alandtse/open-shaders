#include "AssetScan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace FurShellAssets
{
	namespace
	{
		constexpr std::string_view Suffix = "_shell.dds";

		bool EndsWithShell(std::string_view a_name)
		{
			return a_name.size() >= Suffix.size() && std::ranges::equal(a_name.substr(a_name.size() - Suffix.size()), Suffix, [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == b;
			});
		}

		bool LooseShellExists(const std::filesystem::path& a_dataDir)
		{
			std::error_code ec;
			std::filesystem::recursive_directory_iterator it(a_dataDir / "textures", std::filesystem::directory_options::skip_permission_denied, ec);
			for (const std::filesystem::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
				if (it->is_regular_file(ec) && EndsWithShell(it->path().filename().string()))
					return true;
			}
			return false;
		}

		struct Header
		{
			char magic[4];
			uint32_t version;
			uint32_t folderRecordOffset;
			uint32_t archiveFlags;
			uint32_t folderCount;
			uint32_t fileCount;
			uint32_t totalFolderNameLength;
			uint32_t totalFileNameLength;
			uint32_t fileFlags;
		};
		static_assert(sizeof(Header) == 36);

		constexpr uint32_t IncludeDirectoryNames = 0x1;
		constexpr uint32_t IncludeFileNames = 0x2;
		constexpr uint32_t FileRecordSize = 16;

		/** @return True when the archive holds a shell or its file name table cannot be read. */
		bool ArchiveMayHoldShell(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary);
			Header header{};
			if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || std::string_view(header.magic, 4) != std::string_view("BSA\0", 4))
				return true;
			if ((header.archiveFlags & IncludeFileNames) == 0 || (header.version != 104 && header.version != 105))
				return true;

			const uint64_t folderRecordSize = header.version == 105 ? 24 : 16;
			uint64_t nameTable = header.folderRecordOffset + header.folderCount * folderRecordSize + static_cast<uint64_t>(header.fileCount) * FileRecordSize;
			if (header.archiveFlags & IncludeDirectoryNames)
				nameTable += header.totalFolderNameLength + header.folderCount;  // each folder name carries a length byte

			std::string names(header.totalFileNameLength, '\0');
			file.seekg(static_cast<std::streamoff>(nameTable));
			if (!file.read(names.data(), static_cast<std::streamsize>(names.size())))
				return true;

			for (size_t begin = 0; begin < names.size();) {
				const size_t end = names.find('\0', begin);
				if (end == std::string::npos || EndsWithShell(std::string_view(names).substr(begin, end - begin)))
					return true;
				begin = end + 1;
			}
			return false;
		}
	}

	bool AnyShellTextures(const std::filesystem::path& a_dataDir)
	{
		if (LooseShellExists(a_dataDir))
			return true;

		std::error_code ec;
		for (std::filesystem::directory_iterator it(a_dataDir, ec), end; !ec && it != end; it.increment(ec)) {
			const auto& path = it->path();
			if (_stricmp(path.extension().string().c_str(), ".bsa") == 0 && ArchiveMayHoldShell(path))
				return true;
		}
		return static_cast<bool>(ec);
	}
}
