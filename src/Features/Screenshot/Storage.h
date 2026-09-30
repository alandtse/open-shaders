#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace OS::CaptureStorage
{
	struct CommittedArtifact
	{
		std::uint64_t bytes = 0;
		std::string sha256;
	};

	/** Holds a committed regular file stable while its size and digest are read. */
	class CommittedFile final
	{
	public:
		/** Locks an existing regular file for stable integrity checks. */
		static CommittedFile Open(const std::filesystem::path& a_path);
		/** Publishes a new owned temporary file and verifies the destination identity. */
		static CommittedArtifact WriteAtomically(
			const std::filesystem::path& a_temporaryPath,
			const std::filesystem::path& a_destination,
			const void* a_data,
			std::size_t a_size,
			bool a_replaceExisting);

		/** Transfers ownership of the locked file. */
		CommittedFile(CommittedFile&& a_other) noexcept;
		/** Replaces this ownership with another locked file. */
		CommittedFile& operator=(CommittedFile&& a_other) noexcept;
		/** Releases the file handle. */
		~CommittedFile();

		CommittedFile(const CommittedFile&) = delete;
		CommittedFile& operator=(const CommittedFile&) = delete;

		/** Reads size and digest while verifying that the file identity stays fixed. */
		CommittedArtifact Describe() const;

	private:
		explicit CommittedFile(void* a_handle, std::filesystem::path a_path);
		void Release() noexcept;

		void* handle = nullptr;
		std::filesystem::path path;
	};

	/**
	 * Owns an exclusively-created sequence directory and prevents its path from
	 * being renamed or replaced until all frame and manifest writes finish.
	 */
	class DirectoryLease final
	{
	public:
		/** Creates an exclusive request subdirectory and locks its resolved parent. */
		static std::shared_ptr<DirectoryLease> CreateExclusive(
			const std::filesystem::path& a_destination,
			std::string_view a_requestId);

		/** Verifies a reference file lies directly in the locked destination directory. */
		void VerifyDestinationChild(const std::filesystem::path& a_path) const;
		/** Releases path ownership without deleting captured evidence. */
		~DirectoryLease();

		DirectoryLease(const DirectoryLease&) = delete;
		DirectoryLease& operator=(const DirectoryLease&) = delete;

		/** Returns the owned directory's resolved physical path. */
		const std::filesystem::path& Path() const noexcept { return path; }
		/** Rejects changes to either directory identity or resolved path. */
		void Verify() const;
		/** Verifies ownership and direct-child containment before publication. */
		void VerifyDirectChild(const std::filesystem::path& a_path) const;

	private:
		void VerifyChild(const std::filesystem::path& a_path, const std::filesystem::path& a_parent) const;
		DirectoryLease(
			void* a_destinationHandle,
			void* a_directoryHandle,
			std::filesystem::path a_destination,
			std::filesystem::path a_path,
			std::string a_destinationIdentity,
			std::string a_directoryIdentity);

		void* destinationHandle = nullptr;
		void* directoryHandle = nullptr;
		std::filesystem::path destination;
		std::filesystem::path path;
		std::string destinationIdentity;
		std::string directoryIdentity;
	};
}
