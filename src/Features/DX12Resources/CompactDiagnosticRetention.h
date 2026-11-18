#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

namespace DX12ResourceService
{
	inline constexpr uintmax_t kCompactDiagnosticBudgetBytes = 64ull * 1024 * 1024;
	inline constexpr size_t kCompactDiagnosticFileLimit = 256;
	inline constexpr uintmax_t kGeneratedLogBudgetBytes = 8ull * 1024 * 1024;
	inline constexpr size_t kGeneratedLogFileLimit = 16;

	inline bool IsCompactDiagnosticFile(const std::filesystem::path& path)
	{
		const auto name = path.filename().string();
		if (name.size() != 11 || name.substr(6) != ".json")
			return false;
		return std::all_of(name.begin(), name.begin() + 6, [](char digit) {
			return (digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F');
		});
	}

	inline size_t PruneCompactDiagnostics(const std::filesystem::path& root,
		uintmax_t maxBytes = kCompactDiagnosticBudgetBytes, size_t maxFiles = kCompactDiagnosticFileLimit)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec) || ec ||
			std::filesystem::is_symlink(std::filesystem::symlink_status(root, ec)) || ec)
			return 0;
		struct File
		{
			std::filesystem::path path;
			std::filesystem::file_time_type modified;
			uintmax_t bytes = 0;
		};
		std::vector<File> files;
		uintmax_t used = 0;
		std::filesystem::recursive_directory_iterator it(root,
			std::filesystem::directory_options::skip_permission_denied, ec), end;
		for (; !ec && it != end; it.increment(ec)) {
			if (it.depth() > 2) {
				it.disable_recursion_pending();
				continue;
			}
			if (it.depth() != 2 || !IsCompactDiagnosticFile(it->path()) ||
				!it->is_regular_file(ec) || ec)
				continue;
			const auto bytes = it->file_size(ec);
			if (ec || bytes > std::numeric_limits<uintmax_t>::max() - used)
				continue;
			const auto modified = it->last_write_time(ec);
			if (ec)
				continue;
			used += bytes;
			files.push_back({ it->path(), modified, bytes });
		}
		std::ranges::sort(files, [](const auto& left, const auto& right) {
			return left.modified == right.modified ? left.path < right.path : left.modified < right.modified;
		});
		size_t removed = 0;
		for (const auto& file : files) {
			if (used <= maxBytes && files.size() - removed <= maxFiles)
				break;
			ec.clear();
			if (std::filesystem::remove(file.path, ec) && !ec) {
				used -= file.bytes;
				++removed;
			}
		}
		return removed;
	}

	inline size_t PruneGeneratedJobFiles(const std::filesystem::path& folder,
		uintmax_t maxBytes = kGeneratedLogBudgetBytes, size_t maxFiles = kGeneratedLogFileLimit)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(folder, ec) || ec ||
			std::filesystem::is_symlink(std::filesystem::symlink_status(folder, ec)) || ec)
			return 0;
		struct File
		{
			std::filesystem::path path;
			std::filesystem::file_time_type modified;
			uintmax_t bytes;
		};
		std::vector<File> files;
		uintmax_t used = 0;
		std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
		for (; !ec && it != end; it.increment(ec)) {
			const auto path = it->path();
			const auto name = path.filename().string();
			const bool generated = (path.extension() == ".log" &&
				(name.starts_with("prepare-") || name.starts_with("interiors-"))) ||
				(path.extension() == ".json" && name.starts_with("interiors-"));
			if (!generated || !it->is_regular_file(ec) || ec)
				continue;
			const auto bytes = it->file_size(ec);
			if (ec || bytes > std::numeric_limits<uintmax_t>::max() - used)
				continue;
			const auto modified = it->last_write_time(ec);
			if (ec)
				continue;
			used += bytes;
			files.push_back({ path, modified, bytes });
		}
		std::ranges::sort(files, [](const auto& left, const auto& right) {
			return left.modified == right.modified ? left.path < right.path : left.modified < right.modified;
		});
		size_t removed = 0;
		for (const auto& file : files) {
			if (used <= maxBytes && files.size() - removed <= maxFiles)
				break;
			ec.clear();
			if (std::filesystem::remove(file.path, ec) && !ec) {
				used -= file.bytes;
				++removed;
			}
		}
		return removed;
	}
}
