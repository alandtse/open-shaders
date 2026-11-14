#pragma once

#include "RuntimeGeometryCapture.h"
#include "Utils/FileDigest.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace DX12Native
{
	/** @brief Versioned, byte-exact CPU geometry cache payload.
     *
     * Cache keys cover geometry data AND its reference/shape/transform/bounds.
     * Persisted bytes are NEVER treated as a live instance without a fresh,
     * byte-identical runtime observation and the usual material identity checks.
     * captureFrame is intentionally excluded because it is transient.
     */
	namespace GeometryCacheCodec
	{
		constexpr uint32_t kMagic = 0x50434752u;  // RGCP
		constexpr uint32_t kVersion = 1;
		constexpr uint32_t kMaximumSourceBytes = 512 * 1024;

		inline void Put(std::vector<uint8_t>& out, const void* data, size_t size)
		{
			const auto* begin = static_cast<const uint8_t*>(data);
			out.insert(out.end(), begin, begin + size);
		}
		inline void Put32(std::vector<uint8_t>& out, uint32_t value)
		{
			Put(out, &value, sizeof(value));
		}
		inline bool Get(std::span<const uint8_t> data, size_t& cursor, void* out, size_t count)
		{
			if (cursor > data.size() || count > data.size() - cursor)
				return false;
			std::memcpy(out, data.data() + cursor, count);
			cursor += count;
			return true;
		}
		inline bool Get32(std::span<const uint8_t> data, size_t& cursor, uint32_t& out)
		{
			return Get(data, cursor, &out, sizeof(out));
		}
		inline bool ValidPacket(const RuntimeGeometryPacket& packet)
		{
			if (!packet.indexCount || packet.indexCount % 3 ||
				packet.indexCount != packet.indices.size() ||
				packet.vertices.size() != uint64_t(packet.vertexCount) * packet.vertexStride)
				return false;
			RuntimeGeometryInput input;
			input.referenceKey = packet.referenceKey;
			input.shapeName = packet.shapeName;
			input.transform = packet.transform;
			input.bound = packet.bound;
			input.vertexCount = packet.vertexCount;
			input.vertexStride = packet.vertexStride;
			input.triangleCount = packet.indexCount / 3;
			input.hasNormal = packet.hasNormal;
			input.hasUv = packet.hasUv;
			input.hasTangent = packet.hasTangent;
			input.normalOffset = packet.normalOffset;
			input.uvOffset = packet.uvOffset;
			input.tangentOffset = packet.tangentOffset;
			input.vertices = packet.vertices;
			input.indices = packet.indices;
			return ValidRuntimeGeometryInput(input, kMaximumSourceBytes);
		}
	}

	/** @brief Deterministic, size-capped, little-endian Win64 geometry serialization. */
	inline std::optional<std::vector<uint8_t>> SerializeRuntimeGeometryCache(const RuntimeGeometryPacket& packet)
	{
		using namespace GeometryCacheCodec;
		if (!ValidPacket(packet))
			return std::nullopt;
		std::vector<uint8_t> data;
		data.reserve(108 + packet.referenceKey.size() + packet.shapeName.size() +
					 packet.vertices.size() + packet.indices.size() * sizeof(uint16_t));
		Put32(data, kMagic);
		Put32(data, kVersion);
		Put32(data, uint32_t(packet.referenceKey.size()));
		Put32(data, uint32_t(packet.shapeName.size()));
		Put32(data, packet.vertexCount);
		Put32(data, packet.vertexStride);
		Put32(data, packet.indexCount);
		Put32(data, packet.normalOffset);
		Put32(data, packet.uvOffset);
		Put32(data, packet.tangentOffset);
		const uint32_t flags = (packet.hasNormal ? 1u : 0u) | (packet.hasUv ? 2u : 0u) |
		                       (packet.hasTangent ? 4u : 0u);
		Put32(data, flags);
		Put(data, packet.transform.data(), sizeof(packet.transform));
		Put(data, packet.bound.data(), sizeof(packet.bound));
		Put(data, packet.referenceKey.data(), packet.referenceKey.size());
		Put(data, packet.shapeName.data(), packet.shapeName.size());
		Put(data, packet.vertices.data(), packet.vertices.size());
		Put(data, packet.indices.data(), packet.indices.size() * sizeof(uint16_t));
		return data;
	}

	/** @brief Refuses corrupt/oversized caches before assigning any runtime geometry. */
	inline std::shared_ptr<const RuntimeGeometryPacket> DeserializeRuntimeGeometryCache(
		std::span<const uint8_t> bytes)
	{
		using namespace GeometryCacheCodec;
		if (bytes.size() < 108 || bytes.size() > kMaximumSourceBytes + 4096)
			return {};
		size_t cursor = 0;
		uint32_t magic = 0, version = 0, refLength = 0, shapeLength = 0, flags = 0;
		auto packet = std::make_shared<RuntimeGeometryPacket>();
		if (!Get32(bytes, cursor, magic) || !Get32(bytes, cursor, version) ||
			!Get32(bytes, cursor, refLength) || !Get32(bytes, cursor, shapeLength) ||
			!Get32(bytes, cursor, packet->vertexCount) ||
			!Get32(bytes, cursor, packet->vertexStride) ||
			!Get32(bytes, cursor, packet->indexCount) ||
			!Get32(bytes, cursor, packet->normalOffset) ||
			!Get32(bytes, cursor, packet->uvOffset) ||
			!Get32(bytes, cursor, packet->tangentOffset) ||
			!Get32(bytes, cursor, flags) ||
			magic != kMagic || version != kVersion || (flags & ~7u) ||
			!refLength || refLength > 256 || !shapeLength || shapeLength > 1024)
			return {};
		if (!Get(bytes, cursor, packet->transform.data(), sizeof(packet->transform)) ||
			!Get(bytes, cursor, packet->bound.data(), sizeof(packet->bound)))
			return {};
		const uint64_t vertices = uint64_t(packet->vertexCount) * packet->vertexStride;
		const uint64_t indices = uint64_t(packet->indexCount) * sizeof(uint16_t);
		const uint64_t required = uint64_t(refLength) + shapeLength + vertices + indices;
		if (required > bytes.size() - cursor || required != bytes.size() - cursor ||
			vertices > kMaximumSourceBytes || indices > kMaximumSourceBytes ||
			vertices + indices > kMaximumSourceBytes)
			return {};
		packet->referenceKey.resize(refLength);
		packet->shapeName.resize(shapeLength);
		if (!Get(bytes, cursor, packet->referenceKey.data(), refLength) ||
			!Get(bytes, cursor, packet->shapeName.data(), shapeLength))
			return {};
		if (packet->referenceKey.find('\0') != std::string::npos ||
			packet->shapeName.find('\0') != std::string::npos)
			return {};
		packet->vertices.resize(size_t(vertices));
		packet->indices.resize(packet->indexCount);
		if (!Get(bytes, cursor, packet->vertices.data(), size_t(vertices)) ||
			!Get(bytes, cursor, packet->indices.data(), size_t(indices)))
			return {};
		packet->hasNormal = (flags & 1u) != 0;
		packet->hasUv = (flags & 2u) != 0;
		packet->hasTangent = (flags & 4u) != 0;
		return ValidPacket(*packet) ? packet : nullptr;
	}

	/** @brief Hash used for safe reuse after matching an actually observed mesh. */
	inline std::optional<std::string> RuntimeGeometryContentId(const RuntimeGeometryPacket& packet)
	{
		const auto bytes = SerializeRuntimeGeometryCache(packet);
		return bytes ? Util::FileDigest::Sha256BytesHex(*bytes) : std::nullopt;
	}
}
