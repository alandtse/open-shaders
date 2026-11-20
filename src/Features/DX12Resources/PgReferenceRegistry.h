#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace DX12ResourceService
{
	struct PgReferenceSnapshot
	{
		std::string sourceDiffSha256;
		uint32_t meshCount = 0;
		uint32_t shapeCount = 0;
		uint32_t cacheHits = 0;
		uint32_t unresolved = 0;
		std::unordered_map<std::string, nlohmann::json> meshes;

		/** @brief Read-only lookup by PG-deployed mesh path; the returned entry lives with the snapshot. */
		const nlohmann::json* FindMesh(std::string_view path) const;
	};

	inline std::string NormalizePgMeshPath(std::string_view input)
	{
		std::string result;
		result.reserve(input.size() + 7);
		for (unsigned char c : input)
			result.push_back(c == '/' ? '\\' : char(std::tolower(c)));
		if (!result.starts_with("meshes\\"))
			result.insert(0, "meshes\\");
		if (!result.ends_with(".nif") || result.size() > 512)
			return {};
		for (size_t start = 0; start < result.size();) {
			const auto end = result.find('\\', start);
			const auto part = result.substr(start, end == std::string::npos ? end : end - start);
			if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos)
				return {};
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
		return result;
	}

	inline const nlohmann::json* PgReferenceSnapshot::FindMesh(std::string_view path) const
	{
		const auto normalized = NormalizePgMeshPath(path);
		const auto found = meshes.find(normalized);
		return found == meshes.end() ? nullptr : &found->second;
	}

	inline bool ValidPgDigest(std::string_view digest)
	{
		return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		});
	}

	/** @brief Validate and publish immutable references; no geometry or GPU work occurs here. */
	inline std::shared_ptr<const PgReferenceSnapshot> ParsePgReferenceSnapshot(nlohmann::json document)
	{
		if (!document.is_object() || document.value("kind", std::string{}) != "dx12-pg-references" ||
			document.value("schemaVersion", 0) != 1)
			throw std::runtime_error("PG reference contract mismatch");
		auto result = std::make_shared<PgReferenceSnapshot>();
		result->sourceDiffSha256 = document.at("sourceDiffSha256").get<std::string>();
		if (!ValidPgDigest(result->sourceDiffSha256))
			throw std::runtime_error("PG reference source digest invalid");
		auto& entries = document.at("meshes");
		if (!entries.is_array() || entries.size() > 200'000 ||
			entries.size() != document.at("meshCount").get<size_t>())
			throw std::runtime_error("PG reference mesh count mismatch");
		result->meshCount = uint32_t(entries.size());
		result->cacheHits = document.at("cacheHits").get<uint32_t>();
		result->unresolved = document.at("unresolved").get<uint32_t>();
		if (result->cacheHits > result->meshCount || result->unresolved > result->meshCount)
			throw std::runtime_error("PG reference counters exceed mesh count");
		uint64_t countedShapes = 0;
		uint32_t unresolved = 0;
		for (auto& mesh : entries) {
			const auto path = mesh.at("path").get<std::string>();
			const auto normalized = NormalizePgMeshPath(path);
			if (normalized.empty() || normalized != path)
				throw std::runtime_error("PG reference mesh path invalid");
			const auto status = mesh.at("status").get<std::string>();
			if (status != "verified")
				++unresolved;
			else if (!ValidPgDigest(mesh.at("sha256").get<std::string>()))
				throw std::runtime_error("PG reference mesh digest invalid");
			const auto& shapes = mesh.at("shapes");
			if (!shapes.is_array() || shapes.size() > 65'536)
				throw std::runtime_error("PG reference shape list invalid");
			std::unordered_set<uint32_t> seen;
			for (const auto& shape : shapes) {
				const auto ordinal = shape.at("blockIndex").get<uint32_t>();
				if (!seen.insert(ordinal).second ||
					!shape.at("textures").is_array() || shape.at("textures").size() > 16 ||
					!shape.at("shaderFlags1").is_number_unsigned() ||
					!shape.at("shaderFlags2").is_number_unsigned())
					throw std::runtime_error("PG reference shape contract mismatch");
				for (const auto& texture : shape.at("textures"))
					if (!texture.is_string() || texture.get_ref<const std::string&>().size() > 1024)
						throw std::runtime_error("PG reference texture path invalid");
			}
			countedShapes += shapes.size();
			if (countedShapes > 8'000'000 || !result->meshes.emplace(normalized, std::move(mesh)).second)
				throw std::runtime_error("PG reference duplicate or oversized index");
		}
		result->shapeCount = document.at("shapeCount").get<uint32_t>();
		if (result->shapeCount != countedShapes || result->unresolved != unresolved)
			throw std::runtime_error("PG reference summary mismatch");
		return result;
	}

	inline std::shared_ptr<const PgReferenceSnapshot> ReadPgReferenceSnapshot(const std::filesystem::path& path)
	{
		std::error_code error;
		const auto size = std::filesystem::file_size(path, error);
		if (error || size == 0 || size > 256ull * 1024 * 1024)
			throw std::runtime_error("Missing or oversized PG reference index");
		std::ifstream input(path, std::ios::binary);
		if (!input)
			throw std::runtime_error("Cannot open PG reference index");
		return ParsePgReferenceSnapshot(nlohmann::json::parse(input));
	}
}
