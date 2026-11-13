#pragma once

#include "Utils/FileDigest.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace DX12Packed
{
	inline constexpr uint32_t kGeometryMagic = 0x50475352;  // RSGP
	inline constexpr uint32_t kShadingMagic = 0x50535352;   // RSSP
	inline constexpr uint64_t kMaximumPackBytes = 512ull * 1024 * 1024;

	inline bool ValidDigest(const std::string& value)
	{
		return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		});
	}

	inline std::string Hash(std::span<const uint8_t> bytes)
	{
		auto digest = Util::FileDigest::Sha256BytesHex(bytes);
		if (!digest)
			throw std::runtime_error("Packed CELL SHA-256 unavailable");
		std::transform(digest->begin(), digest->end(), digest->begin(),
			[](unsigned char c) { return char(std::tolower(c)); });
		return *digest;
	}

	struct CellPack
	{
		struct Range
		{
			size_t offset, length;
		};
		std::vector<uint8_t> bytes;
		std::unordered_map<std::string, Range> entries;

		std::span<const uint8_t> Get(const std::string& digest) const
		{
			const auto found = entries.find(digest);
			if (found == entries.end())
				throw std::runtime_error("Packed CELL resource missing: " + digest);
			return std::span<const uint8_t>(bytes).subspan(found->second.offset, found->second.length);
		}
	};

	// The JSON descriptor indexes original complete RSGM/RSGA blobs within a single pack.
	// The complete pack and each indexed blob are independently SHA-256 verified.
	inline CellPack Read(const std::filesystem::path& directory, const nlohmann::json& descriptor,
		std::string_view expectedName, uint32_t expectedMagic)
	{
		const auto name = descriptor.at("file").get<std::string>();
		const auto expectedHash = descriptor.at("sha256").get<std::string>();
		const auto length = descriptor.at("byteLength").get<uint64_t>();
		const auto& index = descriptor.at("entries");
		if (name != expectedName || !ValidDigest(expectedHash) || length < 16 ||
			length > kMaximumPackBytes || !index.is_array() || index.size() > 65536)
			throw std::runtime_error("Invalid prepared CELL pack descriptor");
		const auto path = directory / name;
		std::error_code ec;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) || ec ||
			!std::filesystem::is_regular_file(path, ec) || ec ||
			std::filesystem::file_size(path, ec) != length || ec)
			throw std::runtime_error("Prepared CELL pack missing or unsafe");
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input)
			throw std::runtime_error("Cannot open prepared CELL pack");
		CellPack pack;
		pack.bytes.resize(size_t(length));
		input.seekg(0);
		if (!input.read(reinterpret_cast<char*>(pack.bytes.data()), std::streamsize(length)))
			throw std::runtime_error("Incomplete prepared CELL pack read");
		if (Hash(pack.bytes) != expectedHash)
			throw std::runtime_error("Prepared CELL pack SHA-256 mismatch");
		std::array<uint32_t, 4> header{};
		std::memcpy(header.data(), pack.bytes.data(), sizeof(header));
		if (header[0] != expectedMagic || header[1] != 1 || header[2] != index.size() || header[3] != 0)
			throw std::runtime_error("Invalid prepared CELL pack header");
		uint64_t previousEnd = 16;
		for (const auto& item : index) {
			const auto digest = item.at("sha256").get<std::string>();
			const auto offset = item.at("offset").get<uint64_t>();
			const auto bytes = item.at("length").get<uint64_t>();
			if (!ValidDigest(digest) || bytes < 16 || (offset & 15) || offset < previousEnd ||
				offset > length || bytes > length - offset)
				throw std::runtime_error("Invalid prepared CELL pack entry");
			if (Hash(std::span<const uint8_t>(pack.bytes).subspan(size_t(offset), size_t(bytes))) != digest)
				throw std::runtime_error("Packed CELL entry SHA-256 mismatch");
			if (!pack.entries.emplace(digest, CellPack::Range{ size_t(offset), size_t(bytes) }).second)
				throw std::runtime_error("Duplicate prepared CELL pack entry");
			previousEnd = offset + bytes;
		}
		if (previousEnd != length)
			throw std::runtime_error("Prepared CELL pack trailing bytes are not indexed");
		return pack;
	}
}
