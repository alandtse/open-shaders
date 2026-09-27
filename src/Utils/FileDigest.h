#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Util::FileDigest
{
	/**
	 * @brief SHA-256 of a file's raw bytes, as uppercase hex.
	 * @param path File to digest. Raw bytes are hashed without line-ending normalization.
	 * @return The digest, or nullopt on any IO or CNG failure. A caller must treat
	 *         that as "digest unknown", never as "unchanged" or "validated".
	 */
	std::optional<std::string> Sha256FileHex(const std::filesystem::path& path);
}
