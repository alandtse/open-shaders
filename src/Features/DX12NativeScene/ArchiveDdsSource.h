#pragma once

#include "MaterialTextureLookup.h"
#include "RE/B/BSResourceNiBinaryStream.h"
#include "Features/NativePBR/NativePBRCompiler.h"
#include "Utils/FileDigest.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace DX12Native
{
	/** @brief A bounded copy of DDS source bytes, never a borrowed engine pointer. */
	struct ArchiveDdsBytes
	{
		std::vector<uint8_t> bytes;
		std::string digest;
	};

	constexpr size_t kMaximumArchiveDdsBytes = 32ull * 1024 * 1024;

	/** @brief Fail closed unless the requested archive asset is already an exact canonical DDS path. */
	inline bool ValidArchiveDdsPath(const std::string& a_path)
	{
		const auto normalized = NativePBR::CanonicalTexturePath(a_path);
		return normalized && *normalized == a_path && a_path.starts_with("textures/") &&
		       a_path.ends_with(".dds");
	}

	/** @brief Synchronously copies one archive-backed DDS through Skyrim's resource manager.
     *
     * Must be called only on the existing background material loader, never per draw or
     * from the hot render path. The returned bytes are owned and SHA-256 identified.
     * Re-read with expected digest before upload: archives may be replaced on disk.
     */
	inline ArchiveDdsBytes ReadArchiveDds(const std::string& a_canonicalPath,
		std::string_view a_expectedDigest = {})
	{
		if (!ValidArchiveDdsPath(a_canonicalPath))
			throw std::runtime_error("Invalid archive DDS asset path");
		RE::BSResourceNiBinaryStream stream(MaterialTextureLookupKey(a_canonicalPath));
		if (!stream.good() || !stream.stream)
			throw std::runtime_error("Archive DDS resource unavailable");
		const auto total = uint64_t(stream.stream->totalSize);
		if (total < 128 || total > kMaximumArchiveDdsBytes)
			throw std::runtime_error("Archive DDS exceeds source byte limit");
		ArchiveDdsBytes out;
		out.bytes.resize(size_t(total));
		uint64_t bytesRead = 0;
		(void)stream.stream->DoRead(out.bytes.data(), total, bytesRead);
		if (bytesRead != total)
			throw std::runtime_error("Archive DDS read incomplete");
		auto digest = Util::FileDigest::Sha256BytesHex(
			std::span<const uint8_t>(out.bytes.data(), out.bytes.size()));
		if (!digest)
			throw std::runtime_error("Archive DDS digest unavailable");
		std::ranges::transform(*digest, digest->begin(),
			[](unsigned char c) { return char(std::tolower(c)); });
		if (!a_expectedDigest.empty() && *digest != a_expectedDigest)
			throw std::runtime_error("Archive DDS changed before GPU staging");
		out.digest = std::move(*digest);
		return out;
	}
}
