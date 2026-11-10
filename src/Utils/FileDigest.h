#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <span>
#include <cstdint>

// Separate from Utils/ContentHash.h: that one is explicitly non-cryptographic (XXH3, "no
// adversarial threat model applies") and exists for fast shader-cache keys. This file exists to
// validate external content against a cryptographic digest, where a real
// cryptographic hash is the point.
namespace Util::FileDigest
{
	/**
	 * @brief SHA-256 of a file's raw bytes, as uppercase hex.
	 * @param path File to digest. Raw bytes are hashed without line-ending normalization.
	 * @return The digest, or nullopt on any IO or CNG failure. A caller must treat
	 *         that as "digest unknown", never as "unchanged" or "validated".
	 */
	std::optional<std::string> Sha256FileHex(const std::filesystem::path& path);
	/** SHA-256 of an owned synchronous Win32 file handle, uppercase hex; consumes its file position. */
	std::optional<std::string> Sha256HandleHex(void* handle);
	/** @brief SHA-256 of owned bytes, uppercase hex. */
	std::optional<std::string> Sha256BytesHex(std::span<const uint8_t> bytes);
}
