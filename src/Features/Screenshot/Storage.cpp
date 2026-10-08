#include "Features/Screenshot/Storage.h"

#include "Utils/FileDigest.h"
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

namespace OS::CaptureStorage
{
	namespace
	{
		class ScopedHandle final
		{
		public:
			explicit ScopedHandle(HANDLE a_handle = INVALID_HANDLE_VALUE) : handle(a_handle) {}
			~ScopedHandle()
			{
				if (handle != INVALID_HANDLE_VALUE)
					CloseHandle(handle);
			}

			ScopedHandle(const ScopedHandle&) = delete;
			ScopedHandle& operator=(const ScopedHandle&) = delete;

			HANDLE Get() const noexcept { return handle; }
			HANDLE Release() noexcept
			{
				const auto released = handle;
				handle = INVALID_HANDLE_VALUE;
				return released;
			}

		private:
			HANDLE handle;
		};

		std::string Identity(const BY_HANDLE_FILE_INFORMATION& a_information)
		{
			return std::format(
				"{:08x}:{:08x}:{:08x}", a_information.dwVolumeSerialNumber,
				a_information.nFileIndexHigh, a_information.nFileIndexLow);
		}

		BY_HANDLE_FILE_INFORMATION ReadIdentity(HANDLE a_handle, bool a_directory)
		{
			BY_HANDLE_FILE_INFORMATION information{};
			if (!GetFileInformationByHandle(a_handle, &information))
				throw std::runtime_error(std::format("file identity query failed with Win32 error {}", GetLastError()));
			const bool isDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (isDirectory != a_directory ||
				(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
				throw std::runtime_error(a_directory ?
											 "sequence storage is not a stable non-reparse directory" :
											 "committed artifact is not a stable non-reparse regular file");
			}
			return information;
		}

		std::filesystem::path NormalizeFinalPath(std::wstring a_path)
		{
			static constexpr std::wstring_view uncPrefix = L"\\\\?\\UNC\\";
			static constexpr std::wstring_view extendedPrefix = L"\\\\?\\";
			if (a_path.starts_with(uncPrefix))
				a_path.replace(0, uncPrefix.size(), L"\\\\");
			else if (a_path.starts_with(extendedPrefix))
				a_path.erase(0, extendedPrefix.size());
			return std::filesystem::path(a_path).lexically_normal();
		}

		std::filesystem::path FinalPath(HANDLE a_handle)
		{
			const DWORD required = GetFinalPathNameByHandleW(a_handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (required == 0)
				throw std::runtime_error(std::format("final path query failed with Win32 error {}", GetLastError()));
			std::wstring buffer(required, L'\0');
			const DWORD written = GetFinalPathNameByHandleW(
				a_handle, buffer.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (written == 0 || written >= required)
				throw std::runtime_error(std::format("final path query failed with Win32 error {}", GetLastError()));
			buffer.resize(written);
			return NormalizeFinalPath(std::move(buffer));
		}

		bool SamePath(const std::filesystem::path& a_left, const std::filesystem::path& a_right)
		{
			const auto left = a_left.lexically_normal().native();
			const auto right = a_right.lexically_normal().native();
			return _wcsicmp(left.c_str(), right.c_str()) == 0;
		}

		void RenameHandle(
			HANDLE a_handle,
			const std::filesystem::path& a_destination,
			bool a_replaceExisting)
		{
			const auto destination = std::filesystem::absolute(a_destination).lexically_normal().native();
			constexpr auto headerBytes = offsetof(FILE_RENAME_INFO, FileName);
			if (destination.size() > (MAXDWORD - headerBytes - sizeof(wchar_t)) / sizeof(wchar_t))
				throw std::runtime_error("committed artifact destination is too long");
			const auto nameBytes = destination.size() * sizeof(wchar_t);
			// Win32 path conversion requires a terminator beyond the counted name.
			std::vector<std::byte> storage(std::max<std::size_t>(sizeof(FILE_RENAME_INFO), headerBytes + nameBytes + sizeof(wchar_t)));
			auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
			rename->ReplaceIfExists = a_replaceExisting ? TRUE : FALSE;
			rename->RootDirectory = nullptr;
			rename->FileNameLength = static_cast<DWORD>(nameBytes);
			std::memcpy(rename->FileName, destination.data(), nameBytes);
			if (!SetFileInformationByHandle(
					a_handle, FileRenameInfo, rename, static_cast<DWORD>(storage.size()))) {
				throw std::runtime_error(std::format(
					"committed artifact rename failed with Win32 error {}", GetLastError()));
			}
		}

		void DeleteHandle(HANDLE a_handle) noexcept
		{
			FILE_DISPOSITION_INFO disposition{ .DeleteFile = TRUE };
			SetFileInformationByHandle(
				a_handle, FileDispositionInfo, &disposition, sizeof(disposition));
		}

		HANDLE OpenDirectory(const std::filesystem::path& a_path)
		{
			// Attribute-only handles do not enforce the no-delete sharing lease.
			return CreateFileW(
				a_path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		}

	}

	CommittedFile::CommittedFile(void* a_handle, std::filesystem::path a_path) :
		handle(a_handle), path(std::move(a_path))
	{}

	CommittedFile CommittedFile::Open(const std::filesystem::path& a_path)
	{
		ScopedHandle file(CreateFileW(
			a_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
		if (file.Get() == INVALID_HANDLE_VALUE)
			throw std::runtime_error(std::format("could not lock committed artifact for verification (Win32 error {})", GetLastError()));
		ReadIdentity(file.Get(), false);
		const auto openedPath = FinalPath(file.Get());
		return CommittedFile(file.Release(), openedPath);
	}

	CommittedArtifact CommittedFile::WriteAtomically(
		const std::filesystem::path& a_temporaryPath,
		const std::filesystem::path& a_destination,
		const void* a_data,
		std::size_t a_size,
		bool a_replaceExisting)
	{
		if (a_size != 0 && !a_data)
			throw std::runtime_error("committed artifact data is unavailable");
		const auto temporary = std::filesystem::absolute(a_temporaryPath).lexically_normal();
		const auto destination = std::filesystem::absolute(a_destination).lexically_normal();
		if (!SamePath(temporary.parent_path(), destination.parent_path()))
			throw std::runtime_error("committed artifact temporary file is outside its destination directory");

		ScopedHandle file(CreateFileW(
			temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
			nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
		if (file.Get() == INVALID_HANDLE_VALUE)
			throw std::runtime_error(std::format(
				"committed artifact temporary file creation failed with Win32 error {}", GetLastError()));

		try {
			const auto identity = Identity(ReadIdentity(file.Get(), false));
			const auto* bytes = static_cast<const std::byte*>(a_data);
			std::size_t offset = 0;
			while (offset < a_size) {
				const auto remaining = std::min<std::size_t>(a_size - offset, MAXDWORD);
				DWORD written = 0;
				if (!WriteFile(file.Get(), bytes + offset, static_cast<DWORD>(remaining), &written, nullptr) || written == 0)
					throw std::runtime_error(std::format(
						"committed artifact write failed with Win32 error {}", GetLastError()));
				offset += written;
			}
			if (!FlushFileBuffers(file.Get()))
				throw std::runtime_error(std::format(
					"committed artifact flush failed with Win32 error {}", GetLastError()));

			RenameHandle(file.Get(), destination, a_replaceExisting);
			const auto publishedPath = FinalPath(file.Get());
			ScopedHandle published(CreateFileW(
				destination.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
				nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
			if (published.Get() == INVALID_HANDLE_VALUE)
				throw std::runtime_error(std::format("committed artifact publication verification open failed with Win32 error {}", GetLastError()));
			if (Identity(ReadIdentity(file.Get(), false)) != identity ||
				Identity(ReadIdentity(published.Get(), false)) != identity)
				throw std::runtime_error("committed artifact file identity changed during publication");
			const auto verifiedPath = FinalPath(published.Get());
			if (!SamePath(verifiedPath, publishedPath))
				throw std::runtime_error("committed artifact path changed during publication");
			CommittedFile committed(file.Release(), publishedPath);
			return committed.Describe();
		} catch (...) {
			DeleteHandle(file.Get());
			throw;
		}
	}

	CommittedFile::CommittedFile(CommittedFile&& a_other) noexcept :
		handle(std::exchange(a_other.handle, nullptr)), path(std::move(a_other.path))
	{}

	CommittedFile& CommittedFile::operator=(CommittedFile&& a_other) noexcept
	{
		if (this != &a_other) {
			Release();
			handle = std::exchange(a_other.handle, nullptr);
			path = std::move(a_other.path);
		}
		return *this;
	}

	CommittedFile::~CommittedFile()
	{
		Release();
	}

	CommittedArtifact CommittedFile::Describe() const
	{
		if (!handle)
			throw std::runtime_error("committed artifact handle is unavailable");
		const auto nativeHandle = static_cast<HANDLE>(handle);
		const auto before = ReadIdentity(nativeHandle, false);
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(nativeHandle, &size) || size.QuadPart < 0)
			throw std::runtime_error(std::format("committed artifact size query failed with Win32 error {}", GetLastError()));
		const auto identitySize =
			(static_cast<std::uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
		if (identitySize != static_cast<std::uint64_t>(size.QuadPart))
			throw std::runtime_error("committed artifact size metadata disagrees on the locked handle");
		const auto digest = Util::FileDigest::Sha256HandleHex(nativeHandle);
		if (!digest)
			throw std::runtime_error("committed artifact SHA-256 could not be read");
		const auto after = ReadIdentity(nativeHandle, false);
		if (Identity(before) != Identity(after) ||
			before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
			CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0) {
			throw std::runtime_error("committed artifact changed while its integrity metadata was computed");
		}
		return { .bytes = static_cast<std::uint64_t>(size.QuadPart), .sha256 = *digest };
	}

	void CommittedFile::Release() noexcept
	{
		if (handle)
			CloseHandle(static_cast<HANDLE>(handle));
		handle = nullptr;
	}

	DirectoryLease::DirectoryLease(
		void* a_destinationHandle,
		void* a_directoryHandle,
		std::filesystem::path a_destination,
		std::filesystem::path a_path,
		std::string a_destinationIdentity,
		std::string a_directoryIdentity) :
		destinationHandle(a_destinationHandle),
		directoryHandle(a_directoryHandle),
		destination(std::move(a_destination)),
		path(std::move(a_path)),
		destinationIdentity(std::move(a_destinationIdentity)),
		directoryIdentity(std::move(a_directoryIdentity))
	{}

	std::shared_ptr<DirectoryLease> DirectoryLease::CreateExclusive(
		const std::filesystem::path& a_destination,
		std::string_view a_requestId)
	{
		if (a_requestId.empty() || !std::ranges::all_of(a_requestId, [](const unsigned char value) {
				return std::isalnum(value) != 0 || value == '-';
			})) {
			throw std::runtime_error("sequence request identity is not a safe directory suffix");
		}
		std::filesystem::create_directories(a_destination);
		const auto requestedDestination = std::filesystem::absolute(a_destination).lexically_normal();

		ScopedHandle destinationHandle(OpenDirectory(requestedDestination));
		if (destinationHandle.Get() == INVALID_HANDLE_VALUE)
			throw std::runtime_error(std::format("sequence destination could not be locked (Win32 error {})", GetLastError()));
		const auto destinationInformation = ReadIdentity(destinationHandle.Get(), true);
		const auto destination = FinalPath(destinationHandle.Get());

		const auto directory = destination / ("CS_sequence_" + std::string(a_requestId));
		if (!CreateDirectoryW(directory.c_str(), nullptr)) {
			const auto error = GetLastError();
			if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
				throw std::runtime_error("sequence directory already exists for this request identity");
			throw std::runtime_error(std::format("sequence directory creation failed with Win32 error {}", error));
		}

		ScopedHandle directoryHandle(OpenDirectory(directory));
		if (directoryHandle.Get() == INVALID_HANDLE_VALUE) {
			const auto error = GetLastError();
			RemoveDirectoryW(directory.c_str());
			throw std::runtime_error(std::format("sequence directory could not be locked (Win32 error {})", error));
		}
		try {
			const auto directoryInformation = ReadIdentity(directoryHandle.Get(), true);
			const auto openedDirectory = FinalPath(directoryHandle.Get());
			if (!SamePath(openedDirectory, directory))
				throw std::runtime_error("sequence directory changed between creation and ownership");
			return std::shared_ptr<DirectoryLease>(new DirectoryLease(
				destinationHandle.Release(), directoryHandle.Release(), destination, openedDirectory,
				Identity(destinationInformation), Identity(directoryInformation)));
		} catch (...) {
			CloseHandle(directoryHandle.Release());
			RemoveDirectoryW(directory.c_str());
			throw;
		}
	}

	DirectoryLease::~DirectoryLease()
	{
		if (directoryHandle)
			CloseHandle(static_cast<HANDLE>(directoryHandle));
		if (destinationHandle)
			CloseHandle(static_cast<HANDLE>(destinationHandle));
	}

	void DirectoryLease::Verify() const
	{
		if (!destinationHandle || !directoryHandle)
			throw std::runtime_error("sequence directory ownership is unavailable");
		const auto destinationInformation = ReadIdentity(static_cast<HANDLE>(destinationHandle), true);
		const auto directoryInformation = ReadIdentity(static_cast<HANDLE>(directoryHandle), true);
		if (Identity(destinationInformation) != destinationIdentity ||
			Identity(directoryInformation) != directoryIdentity ||
			!SamePath(FinalPath(static_cast<HANDLE>(destinationHandle)), destination) ||
			!SamePath(FinalPath(static_cast<HANDLE>(directoryHandle)), path)) {
			throw std::runtime_error("sequence directory identity changed while capture was active");
		}
	}

	void DirectoryLease::VerifyDirectChild(const std::filesystem::path& a_path) const
	{
		VerifyChild(a_path, path);
	}

	void DirectoryLease::VerifyDestinationChild(const std::filesystem::path& a_path) const
	{
		VerifyChild(a_path, destination);
	}

	void DirectoryLease::VerifyChild(const std::filesystem::path& a_path, const std::filesystem::path& a_parent) const
	{
		Verify();
		const auto absolute = std::filesystem::absolute(a_path).lexically_normal();
		ScopedHandle parent(OpenDirectory(absolute.parent_path()));
		if (parent.Get() == INVALID_HANDLE_VALUE)
			throw std::runtime_error(std::format("could not resolve sequence output directory (Win32 error {})", GetLastError()));
		ReadIdentity(parent.Get(), true);
		if (!SamePath(FinalPath(parent.Get()), a_parent))
			throw std::runtime_error("sequence output is not a direct child of its owned directory");
	}
}
